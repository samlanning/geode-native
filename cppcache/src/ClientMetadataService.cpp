/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ClientMetadataService.hpp"

#include <climits>
#include <cstdlib>
#include <iterator>
#include <unordered_set>

#include <geode/FixedPartitionResolver.hpp>

#include "TcrMessage.hpp"
#include "ThinClientPoolDM.hpp"

namespace apache {
namespace geode {
namespace client {

const BucketStatus::clock::time_point BucketStatus::m_noTimeout{};

const char* ClientMetadataService::NC_CMDSvcThread = "NC CMDSvcThread";

ClientMetadataService::~ClientMetadataService() {
  if (m_bucketWaitTimeout > std::chrono::milliseconds::zero()) {
    try {
      std::map<std::string, PRbuckets*>::iterator bi;
      for (bi = m_bucketStatus.begin(); bi != m_bucketStatus.end(); ++bi) {
        delete bi->second;
      }

    } catch (...) {
      LOGINFO("Exception in ClientMetadataService destructor");
    }
  }
}

ClientMetadataService::ClientMetadataService(Pool* pool)
    : m_run(false),
      m_pool(pool),
      m_regionQueue(false)

{
  auto tcrdm = dynamic_cast<ThinClientPoolDM*>(m_pool);
  auto cacheImpl = tcrdm->getConnectionManager().getCacheImpl();
  m_bucketWaitTimeout = cacheImpl->getDistributedSystem()
                            .getSystemProperties()
                            .bucketWaitTimeout();
}

int ClientMetadataService::svc() {
  DistributedSystemImpl::setThreadName(NC_CMDSvcThread);
  LOGINFO("ClientMetadataService started for pool " + m_pool->getName());
  while (m_run) {
    m_regionQueueSema.acquire();
    auto tcrdm = dynamic_cast<ThinClientPoolDM*>(m_pool);
    auto&& cache = tcrdm->getConnectionManager().getCacheImpl();
    while (true) {
      auto&& regionFullPath = m_regionQueue.get();

      if (regionFullPath) {
        while (true) {
          if (m_regionQueue.size() > 0) {
            auto&& nextRegionFullPath = m_regionQueue.get();
            if (nextRegionFullPath != nullptr &&
                nextRegionFullPath->c_str() != nullptr &&
                regionFullPath->compare(nextRegionFullPath->c_str()) == 0) {
            } else {
              // different region; put it back
              m_regionQueue.put(nextRegionFullPath);
              break;
            }
          } else {
            break;
          }
        }
      }

      if (!cache->isCacheDestroyPending() && regionFullPath) {
        getClientPRMetadata(regionFullPath->c_str());
      } else {
        break;
      }
    }
    // while(m_regionQueueSema.tryacquire( ) != -1); // release all
  }
  LOGINFO("ClientMetadataService stopped for pool " + m_pool->getName());
  return 0;
}

void ClientMetadataService::getClientPRMetadata(const char* regionFullPath) {
  if (regionFullPath == nullptr) return;
  ThinClientPoolDM* tcrdm = dynamic_cast<ThinClientPoolDM*>(m_pool);
  if (tcrdm == nullptr) {
    throw IllegalArgumentException(
        "ClientMetaData: pool cast to ThinClientPoolDM failed");
  }
  // That means metadata for the region not found, So only for the first time
  // for a particular region use GetClientPartitionAttributesOp
  // TcrMessage to fetch the metadata and put it into map for later use.send
  // this message to server and get metadata from server.
  TcrMessageReply reply(true, nullptr);
  std::string path(regionFullPath);
  std::shared_ptr<ClientMetadata> cptr = nullptr;
  {
    ReadGuard guard(m_regionMetadataLock);
    RegionMetadataMapType::iterator itr = m_regionMetaDataMap.find(path);
    if (itr != m_regionMetaDataMap.end()) {
      cptr = itr->second;
    }
  }
  std::shared_ptr<ClientMetadata> newCptr = nullptr;

  if (cptr == nullptr) {
    TcrMessageGetClientPartitionAttributes request(
        new DataOutput(tcrdm->getConnectionManager()
                           .getCacheImpl()
                           ->getCache()
                           ->createDataOutput()),
        regionFullPath);
    GfErrType err = tcrdm->sendSyncRequest(request, reply);
    if (err == GF_NOERR &&
        reply.getMessageType() ==
            TcrMessage::RESPONSE_CLIENT_PARTITION_ATTRIBUTES) {
      cptr = std::make_shared<ClientMetadata>(reply.getNumBuckets(),
                                              reply.getColocatedWith(), tcrdm,
                                              reply.getFpaSet());
      if (m_bucketWaitTimeout > std::chrono::milliseconds::zero() &&
          reply.getNumBuckets() > 0) {
        WriteGuard guard(m_PRbucketStatusLock);
        m_bucketStatus[regionFullPath] = new PRbuckets(reply.getNumBuckets());
      }
      LOGDEBUG("ClientMetadata buckets %d ", reply.getNumBuckets());
    }
  }
  if (cptr == nullptr) {
    return;
  }

  auto&& colocatedWith = cptr->getColocatedWith();

  if (colocatedWith.empty()) {
    newCptr = SendClientPRMetadata(regionFullPath, cptr);
    // now we will get new instance so assign it again
    if (newCptr != nullptr) {
      cptr->setPreviousone(nullptr);
      newCptr->setPreviousone(cptr);
      WriteGuard guard(m_regionMetadataLock);
      m_regionMetaDataMap[path] = newCptr;
      LOGINFO("Updated client meta data");
    }
  } else {
    newCptr = SendClientPRMetadata(colocatedWith.c_str(), cptr);

    if (newCptr) {
      cptr->setPreviousone(nullptr);
      newCptr->setPreviousone(cptr);
      // now we will get new instance so assign it again
      WriteGuard guard(m_regionMetadataLock);
      m_regionMetaDataMap[colocatedWith.c_str()] = newCptr;
      m_regionMetaDataMap[path] = newCptr;
      LOGINFO("Updated client meta data");
    }
  }
}
std::shared_ptr<ClientMetadata> ClientMetadataService::SendClientPRMetadata(
    const char* regionPath, std::shared_ptr<ClientMetadata> cptr) {
  ThinClientPoolDM* tcrdm = dynamic_cast<ThinClientPoolDM*>(m_pool);
  if (tcrdm == nullptr) {
    throw IllegalArgumentException(
        "ClientMetaData: pool cast to ThinClientPoolDM failed");
  }
  TcrMessageGetClientPrMetadata request(
      new DataOutput(
          tcrdm->getConnectionManager().getCacheImpl()->createDataOutput()),
      regionPath);
  TcrMessageReply reply(true, nullptr);
  // send this message to server and get metadata from server.
  LOGFINE("Now sending GET_CLIENT_PR_METADATA for getting from server: %s",
          regionPath);
  std::shared_ptr<Region> region = nullptr;
  GfErrType err = tcrdm->sendSyncRequest(request, reply);
  if (err == GF_NOERR &&
      reply.getMessageType() == TcrMessage::RESPONSE_CLIENT_PR_METADATA) {
    region =
        tcrdm->getConnectionManager().getCacheImpl()->getRegion(regionPath);
    if (region != nullptr) {
      LocalRegion* lregion = dynamic_cast<LocalRegion*>(region.get());
      lregion->getRegionStats()->incMetaDataRefreshCount();
    }
    std::vector<BucketServerLocationsType>* metadata = reply.getMetadata();
    if (metadata == nullptr) return nullptr;
    if (metadata->empty()) {
      delete metadata;
      return nullptr;
    }
    auto newCptr = std::make_shared<ClientMetadata>(*cptr);
    for (std::vector<BucketServerLocationsType>::iterator iter =
             metadata->begin();
         iter != metadata->end(); ++iter) {
      if (!(*iter).empty()) {
        newCptr->updateBucketServerLocations((*iter).at(0)->getBucketId(),
                                             (*iter));
      }
    }
    delete metadata;
    return newCptr;
  }
  return nullptr;
}

void ClientMetadataService::getBucketServerLocation(
    const std::shared_ptr<Region>& region,
    const std::shared_ptr<CacheableKey>& key,
    const std::shared_ptr<Cacheable>& value,
    const std::shared_ptr<Serializable>& aCallbackArgument, bool isPrimary,
    std::shared_ptr<BucketServerLocation>& serverLocation, int8_t& version) {
  if (region != nullptr) {
    ReadGuard guard(m_regionMetadataLock);
    LOGDEBUG(
        "ClientMetadataService::getBucketServerLocation m_regionMetaDataMap "
        "size is %d",
        m_regionMetaDataMap.size());
    std::string path(region->getFullPath());
    std::shared_ptr<ClientMetadata> cptr = nullptr;
    const auto& itr = m_regionMetaDataMap.find(path);
    if (itr != m_regionMetaDataMap.end()) {
      cptr = itr->second;
    }
    if (!cptr) {
      return;
    }
    std::shared_ptr<CacheableKey> resolvekey;
    const auto& resolver = region->getAttributes().getPartitionResolver();

    EntryEvent event(region, key, value, nullptr, aCallbackArgument, false);
    int bucketId = 0;
    if (resolver == nullptr) {
      resolvekey = key;
    } else {
      resolvekey = resolver->getRoutingObject(event);
      if (resolvekey == nullptr) {
        throw IllegalStateException(
            "The RoutingObject returned by PartitionResolver is null.");
      }
    }
    if (auto&& fpResolver =
            std::dynamic_pointer_cast<FixedPartitionResolver>(resolver)) {
      auto&& partition = fpResolver->getPartitionName(event);
      bucketId = cptr->assignFixedBucketId(partition.c_str(), resolvekey);
      if (bucketId == -1) {
        return;
      }
    } else {
      if (cptr->getTotalNumBuckets() > 0) {
        bucketId =
            std::abs(resolvekey->hashcode() % cptr->getTotalNumBuckets());
      }
    }
    cptr->getServerLocation(bucketId, isPrimary, serverLocation, version);
  }
}

std::shared_ptr<ClientMetadata> ClientMetadataService::getClientMetadata(
    const std::string& regionFullPath) {
  ReadGuard guard(m_regionMetadataLock);
  RegionMetadataMapType::iterator regionMetadataIter =
      m_regionMetaDataMap.find(regionFullPath);
  if (regionMetadataIter != m_regionMetaDataMap.end()) {
    return (*regionMetadataIter).second;
  }
  return nullptr;
}

void ClientMetadataService::populateDummyServers(
    const char* regionName, std::shared_ptr<ClientMetadata> cptr) {
  WriteGuard guard(m_regionMetadataLock);
  m_regionMetaDataMap[regionName] = cptr;
}

void ClientMetadataService::enqueueForMetadataRefresh(
    const std::string& regionFullPath, int8_t serverGroupFlag) {
  ThinClientPoolDM* tcrdm = dynamic_cast<ThinClientPoolDM*>(m_pool);
  if (tcrdm == nullptr) {
    throw IllegalArgumentException(
        "ClientMetaData: pool cast to ThinClientPoolDM failed");
  }

  auto cache = tcrdm->getConnectionManager().getCacheImpl();
  auto region = cache->getRegion(regionFullPath);

  std::string serverGroup = tcrdm->getServerGroup();
  if (serverGroup.length() != 0) {
    cache->setServerGroupFlag(serverGroupFlag);
    if (serverGroupFlag == 2) {
      LOGFINER(
          "Network hop but, from within same server-group, so no metadata "
          "fetch from the server");
      return;
    }
  }

  if (region != nullptr) {
    ThinClientRegion* tcrRegion = dynamic_cast<ThinClientRegion*>(region.get());
    {
      TryWriteGuard guardRegionMetaDataRefresh(
          tcrRegion->getMataDataMutex(), tcrRegion->getMetaDataRefreshed());
      if (tcrRegion->getMetaDataRefreshed()) {
        return;
      }
      LOGFINE("Network hop so fetching single hop metadata from the server");
      cache->setNetworkHopFlag(true);
      tcrRegion->setMetaDataRefreshed(true);
      auto tempRegionPath = std::make_shared<std::string>(regionFullPath);
      m_regionQueue.put(tempRegionPath);
      m_regionQueueSema.release();
    }
  }
}
std::shared_ptr<ClientMetadata> ClientMetadataService::getClientMetadata(
    const std::shared_ptr<Region>& region) {
  ReadGuard guard(m_regionMetadataLock);

  const auto& entry = m_regionMetaDataMap.find(region->getFullPath());

  if (entry == m_regionMetaDataMap.end()) {
    return nullptr;
  }

  return entry->second;
}

std::shared_ptr<ClientMetadataService::ServerToFilterMap>
ClientMetadataService::getServerToFilterMap(
    const std::vector<std::shared_ptr<CacheableKey>>& keys,
    const std::shared_ptr<Region>& region, bool isPrimary) {
  auto clientMetadata = getClientMetadata(region);
  if (!clientMetadata) {
    return nullptr;
  }

  auto serverToFilterMap = std::make_shared<ServerToFilterMap>();

  std::vector<std::shared_ptr<CacheableKey>> keysWhichLeft;
  std::map<int, std::shared_ptr<BucketServerLocation>> buckets;

  for (const auto& key : keys) {
    LOGDEBUG("cmds = %s", key->toString().c_str());
    const auto resolver = region->getAttributes().getPartitionResolver();
    std::shared_ptr<CacheableKey> resolveKey;

    if (resolver == nullptr) {
      // client has not registered PartitionResolver
      // Assuming even PR at server side is not using PartitionResolver
      resolveKey = key;
    } else {
      EntryEvent event(region, key, nullptr, nullptr, nullptr, false);
      resolveKey = resolver->getRoutingObject(event);
    }

    int bucketId =
        std::abs(resolveKey->hashcode() % clientMetadata->getTotalNumBuckets());
    std::shared_ptr<std::vector<std::shared_ptr<CacheableKey>>> keyList =
        nullptr;

    const auto& bucketsIter = buckets.find(bucketId);
    if (bucketsIter == buckets.end()) {
      int8_t version = -1;
      // auto serverLocation = std::make_shared<BucketServerLocation>();
      std::shared_ptr<BucketServerLocation> serverLocation = nullptr;
      clientMetadata->getServerLocation(bucketId, isPrimary, serverLocation,
                                        version);
      if (!(serverLocation && serverLocation->isValid())) {
        keysWhichLeft.push_back(key);
        continue;
      }

      buckets[bucketId] = serverLocation;

      const auto& itrRes = serverToFilterMap->find(serverLocation);

      if (itrRes == serverToFilterMap->end()) {
        keyList =
            std::make_shared<std::vector<std::shared_ptr<CacheableKey>>>();
        serverToFilterMap->emplace(serverLocation, keyList);
      } else {
        keyList = itrRes->second;
      }

      LOGDEBUG("new keylist buckets =%d res = %d", buckets.size(),
               serverToFilterMap->size());
    } else {
      keyList = (*serverToFilterMap)[bucketsIter->second];
    }

    keyList->push_back(key);
  }

  if (keysWhichLeft.size() > 0 &&
      serverToFilterMap->size() > 0) {  // add left keys in result
    auto keyLefts = keysWhichLeft.size();
    auto totalServers = serverToFilterMap->size();
    auto perServer = keyLefts / totalServers + 1;

    size_t keyIdx = 0;
    for (const auto& locationIter : *serverToFilterMap) {
      const auto keys = locationIter.second;
      for (size_t i = 0; i < perServer; i++) {
        if (keyIdx < keyLefts) {
          keys->push_back(keysWhichLeft.at(keyIdx++));
        } else {
          break;
        }
      }
      if (keyIdx >= keyLefts) break;  // done
    }
  } else if (serverToFilterMap->size() == 0) {  // not be able to map any key
    return nullptr;  // it will force all keys to send to one server
  }

  return serverToFilterMap;
}

void ClientMetadataService::markPrimaryBucketForTimeout(
    const std::shared_ptr<Region>& region,
    const std::shared_ptr<CacheableKey>& key,
    const std::shared_ptr<Cacheable>& value,
    const std::shared_ptr<Serializable>& aCallbackArgument, bool,
    std::shared_ptr<BucketServerLocation>& serverLocation, int8_t& version) {
  if (m_bucketWaitTimeout == std::chrono::milliseconds::zero()) return;

  WriteGuard guard(m_PRbucketStatusLock);

  getBucketServerLocation(region, key, value, aCallbackArgument,
                          false /*look for secondary host*/, serverLocation,
                          version);

  if (serverLocation && serverLocation->isValid()) {
    LOGDEBUG("Server host and port are %s:%d",
             serverLocation->getServerName().c_str(),
             serverLocation->getPort());
    int32_t bId = serverLocation->getBucketId();

    std::map<std::string, PRbuckets*>::iterator bs =
        m_bucketStatus.find(region->getFullPath());

    if (bs != m_bucketStatus.end()) {
      bs->second->setBucketTimeout(bId);
      LOGDEBUG("marking bucket %d as timeout ", bId);
    }
  }
}

std::shared_ptr<ClientMetadataService::BucketToKeysMap>
ClientMetadataService::groupByBucketOnClientSide(
    const std::shared_ptr<Region>& region,
    const std::shared_ptr<CacheableVector>& keySet,
    const std::shared_ptr<ClientMetadata>& metadata) {
  auto bucketToKeysMap = std::make_shared<BucketToKeysMap>();
  for (const auto& k : *keySet) {
    const auto key = std::dynamic_pointer_cast<CacheableKey>(k);
    const auto resolver = region->getAttributes().getPartitionResolver();
    std::shared_ptr<CacheableKey> resolvekey;
    EntryEvent event(region, key, nullptr, nullptr, nullptr, false);
    int bucketId = -1;
    if (resolver) {
      resolvekey = resolver->getRoutingObject(event);
      if (!resolvekey) {
        throw IllegalStateException(
            "The RoutingObject returned by PartitionResolver is null.");
      }
    } else {
      resolvekey = key;
    }

    if (auto&& fpResolver =
            std::dynamic_pointer_cast<FixedPartitionResolver>(resolver)) {
      auto&& partition = fpResolver->getPartitionName(event);
      bucketId = metadata->assignFixedBucketId(partition.c_str(), resolvekey);
      if (bucketId == -1) {
        this->enqueueForMetadataRefresh(region->getFullPath(), 0);
      }
    } else {
      if (metadata->getTotalNumBuckets() > 0) {
        bucketId =
            std::abs(resolvekey->hashcode() % metadata->getTotalNumBuckets());
      }
    }

    std::shared_ptr<CacheableHashSet> bucketKeys;

    const auto& iter = bucketToKeysMap->find(bucketId);
    if (iter == bucketToKeysMap->end()) {
      bucketKeys = CacheableHashSet::create();
      bucketToKeysMap->emplace(bucketId, bucketKeys);
    } else {
      bucketKeys = iter->second;
    }

    bucketKeys->insert(key);
  }

  return bucketToKeysMap;
}

std::shared_ptr<ClientMetadataService::ServerToKeysMap>
ClientMetadataService::getServerToFilterMapFESHOP(
    const std::shared_ptr<CacheableVector>& routingKeys,
    const std::shared_ptr<Region>& region, bool isPrimary) {
  auto cptr = getClientMetadata(region->getFullPath());

  if (!cptr) {
    enqueueForMetadataRefresh(region->getFullPath(), 0);
    return nullptr;
  }

  if (!routingKeys) {
    return nullptr;
  }

  const auto bucketToKeysMap =
      groupByBucketOnClientSide(region, routingKeys, cptr);
  BucketSet bucketSet(bucketToKeysMap->size());
  for (const auto& iter : *bucketToKeysMap) {
    bucketSet.insert(iter.first);
  }
  LOGDEBUG(
      "ClientMetadataService::getServerToFilterMapFESHOP: bucketSet size = %d ",
      bucketSet.size());

  const auto serverToBuckets =
      groupByServerToBuckets(cptr, bucketSet, isPrimary);

  if (serverToBuckets == nullptr) {
    return nullptr;
  }

  auto serverToKeysMap = std::make_shared<ServerToKeysMap>();

  for (const auto& serverToBucket : *serverToBuckets) {
    const auto& serverLocation = serverToBucket.first;
    const auto& buckets = serverToBucket.second;
    for (const auto& bucket : *buckets) {
      std::shared_ptr<CacheableHashSet> serverToKeysEntry;
      const auto& iter = serverToKeysMap->find(serverLocation);
      if (iter == serverToKeysMap->end()) {
        serverToKeysEntry = CacheableHashSet::create();
        serverToKeysMap->emplace(serverLocation, serverToKeysEntry);
      } else {
        serverToKeysEntry = iter->second;
      }

      const auto& bucketToKeys = bucketToKeysMap->find(bucket);
      if (bucketToKeys != bucketToKeysMap->end()) {
        const auto& bucketKeys = bucketToKeys->second;
        serverToKeysEntry->insert(bucketKeys->begin(), bucketKeys->end());
      }
    }
  }
  return serverToKeysMap;
}
std::shared_ptr<BucketServerLocation> ClientMetadataService::findNextServer(
    const ClientMetadataService::ServerToBucketsMap& serverToBucketsMap,
    const ClientMetadataService::BucketSet& currentBucketSet) {
  size_t max = 0;
  std::vector<std::shared_ptr<BucketServerLocation>> nodesOfEqualSize;

  for (const auto& serverToBucketEntry : serverToBucketsMap) {
    const auto& serverLocation = serverToBucketEntry.first;
    BucketSet buckets(*(serverToBucketEntry.second));

    LOGDEBUG(
        "ClientMetadataService::findNextServer currentBucketSet->size() = %d  "
        "bucketSet->size() = %d ",
        currentBucketSet.size(), buckets.size());

    for (const auto& currentBucketSetIter : currentBucketSet) {
      buckets.erase(currentBucketSetIter);
      LOGDEBUG("ClientMetadataService::findNextServer bucketSet->size() = %d ",
               buckets.size());
    }

    auto size = buckets.size();
    if (max < size) {
      max = size;
      nodesOfEqualSize.clear();
      nodesOfEqualSize.push_back(serverLocation);
    } else if (max == size) {
      nodesOfEqualSize.push_back(serverLocation);
    }
  }

  auto nodeSize = nodesOfEqualSize.size();
  if (nodeSize > 0) {
    RandGen randgen;
    auto random = randgen(nodeSize);
    return nodesOfEqualSize.at(random);
  }
  return nullptr;
}

std::shared_ptr<ClientMetadataService::ServerToBucketsMap>
ClientMetadataService::pruneNodes(
    const std::shared_ptr<ClientMetadata>& metadata, const BucketSet& buckets) {
  BucketSet bucketSetWithoutServer;
  ServerToBucketsMap serverToBucketsMap;

  auto prunedServerToBucketsMap = std::make_shared<ServerToBucketsMap>();

  for (const auto& bucketId : buckets) {
    const auto locations = metadata->adviseServerLocations(bucketId);
    if (locations.size() == 0) {
      LOGDEBUG(
          "ClientMetadataService::pruneNodes Since no server location "
          "available for bucketId = %d  putting it into "
          "bucketSetWithoutServer ",
          bucketId);
      bucketSetWithoutServer.insert(bucketId);
      continue;
    }

    for (const auto& location : locations) {
      std::shared_ptr<BucketSet> bucketSet;

      const auto& itrRes = serverToBucketsMap.find(location);
      if (itrRes == serverToBucketsMap.end()) {
        bucketSet = std::make_shared<BucketSet>();
        serverToBucketsMap.emplace(location, bucketSet);
      } else {
        bucketSet = itrRes->second;
      }

      bucketSet->insert(bucketId);
    }
  }

  auto itrRes = serverToBucketsMap.begin();
  std::shared_ptr<BucketServerLocation> randomFirstServer;
  if (serverToBucketsMap.empty()) {
    LOGDEBUG(
        "ClientMetadataService::pruneNodes serverToBucketsMap is empty so "
        "returning nullptr");
    return nullptr;
  } else {
    size_t size = serverToBucketsMap.size();
    LOGDEBUG(
        "ClientMetadataService::pruneNodes Total size of serverToBucketsMap = "
        "%d ",
        size);
    for (size_t idx = 0; idx < RandGen{}(size); idx++) {
      itrRes++;
    }
    randomFirstServer = itrRes->first;
  }
  const auto& itrRes1 = serverToBucketsMap.find(randomFirstServer);
  const auto bucketSet = itrRes1->second;

  BucketSet currentBucketSet(*bucketSet);

  prunedServerToBucketsMap->emplace(randomFirstServer, bucketSet);
  serverToBucketsMap.erase(randomFirstServer);

  while (buckets != currentBucketSet) {
    auto server = findNextServer(serverToBucketsMap, currentBucketSet);
    if (server == nullptr) {
      LOGDEBUG(
          "ClientMetadataService::pruneNodes findNextServer returned no "
          "server");
      break;
    }

    const auto& bucketSet2 = serverToBucketsMap.find(server)->second;
    LOGDEBUG(
        "ClientMetadataService::pruneNodes currentBucketSet->size() = %d  "
        "bucketSet2->size() = %d ",
        currentBucketSet.size(), bucketSet2->size());

    for (const auto& currentBucketSetIter : currentBucketSet) {
      bucketSet2->erase(currentBucketSetIter);
      LOGDEBUG("ClientMetadataService::pruneNodes bucketSet2->size() = %d ",
               bucketSet2->size());
    }

    if (bucketSet2->empty()) {
      LOGDEBUG(
          "ClientMetadataService::pruneNodes bucketSet2 is empty() so removing "
          "server from serverToBucketsMap");
      serverToBucketsMap.erase(server);
      continue;
    }

    for (const auto& itr : *bucketSet2) {
      currentBucketSet.insert(itr);
    }

    prunedServerToBucketsMap->emplace(server, bucketSet2);
    serverToBucketsMap.erase(server);
  }

  const auto& itrRes2 = prunedServerToBucketsMap->begin();
  for (const auto& itr : bucketSetWithoutServer) {
    itrRes2->second->insert(itr);
  }

  return prunedServerToBucketsMap;
}

std::shared_ptr<ClientMetadataService::ServerToBucketsMap>
ClientMetadataService::groupByServerToAllBuckets(
    const std::shared_ptr<Region>& region, bool optimizeForWrite) {
  auto cptr = getClientMetadata(region->getFullPath());
  if (cptr == nullptr) {
    enqueueForMetadataRefresh(region->getFullPath(), false);
    return nullptr;
  }
  int totalBuckets = cptr->getTotalNumBuckets();
  BucketSet bucketSet(totalBuckets);
  for (int i = 0; i < totalBuckets; i++) {
    bucketSet.insert(i);
  }
  return groupByServerToBuckets(cptr, bucketSet, optimizeForWrite);
}

std::shared_ptr<ClientMetadataService::ServerToBucketsMap>
ClientMetadataService::groupByServerToBuckets(
    const std::shared_ptr<ClientMetadata>& metadata, const BucketSet& bucketSet,
    bool optimizeForWrite) {
  if (optimizeForWrite) {
    auto serverToBucketsMap = std::make_shared<ServerToBucketsMap>();
    BucketSet bucketsWithoutServer(bucketSet.size());
    for (const auto& bucketId : bucketSet) {
      const auto serverLocation =
          metadata->advisePrimaryServerLocation(bucketId);
      if (serverLocation == nullptr) {
        bucketsWithoutServer.insert(bucketId);
        continue;
      } else if (!serverLocation->isValid()) {
        bucketsWithoutServer.insert(bucketId);
        continue;
      }

      std::shared_ptr<BucketSet> buckets;
      const auto& itrRes = serverToBucketsMap->find(serverLocation);
      if (itrRes == serverToBucketsMap->end()) {
        buckets = std::make_shared<BucketSet>();
        serverToBucketsMap->emplace(serverLocation, buckets);
      } else {
        buckets = itrRes->second;
      }

      buckets->insert(bucketId);
    }

    if (!serverToBucketsMap->empty()) {
      const auto& itrRes = serverToBucketsMap->begin();
      for (const auto& itr : bucketsWithoutServer) {
        itrRes->second->insert(itr);
        LOGDEBUG(
            "ClientMetadataService::groupByServerToBuckets inserting "
            "bucketsWithoutServer");
      }
    }
    return serverToBucketsMap;
  } else {
    return pruneNodes(metadata, bucketSet);
  }
}

void ClientMetadataService::markPrimaryBucketForTimeoutButLookSecondaryBucket(
    const std::shared_ptr<Region>& region,
    const std::shared_ptr<CacheableKey>& key,
    const std::shared_ptr<Cacheable>& value,
    const std::shared_ptr<Serializable>& aCallbackArgument, bool,
    std::shared_ptr<BucketServerLocation>& serverLocation, int8_t& version) {
  if (m_bucketWaitTimeout == std::chrono::milliseconds::zero()) return;

  WriteGuard guard(m_PRbucketStatusLock);

  std::map<std::string, PRbuckets*>::iterator bs =
      m_bucketStatus.find(region->getFullPath());

  PRbuckets* prBuckets = nullptr;
  if (bs != m_bucketStatus.end()) {
    prBuckets = bs->second;
  }

  if (prBuckets == nullptr) return;

  getBucketServerLocation(region, key, value, aCallbackArgument, true,
                          serverLocation, version);

  std::shared_ptr<ClientMetadata> cptr = nullptr;
  {
    ReadGuard guard(m_regionMetadataLock);
    RegionMetadataMapType::iterator cptrIter =
        m_regionMetaDataMap.find(region->getFullPath());

    if (cptrIter != m_regionMetaDataMap.end()) {
      cptr = cptrIter->second;
    }

    if (cptr == nullptr) {
      return;
    }
  }

  LOGFINE("Setting in markPrimaryBucketForTimeoutButLookSecondaryBucket");

  int32_t totalBuckets = cptr->getTotalNumBuckets();

  for (int32_t i = 0; i < totalBuckets; i++) {
    int8_t version;
    std::shared_ptr<BucketServerLocation> bsl;
    cptr->getServerLocation(i, false, bsl, version);

    if (bsl == serverLocation) {
      prBuckets->setBucketTimeout(i);
      LOGFINE(
          "markPrimaryBucketForTimeoutButLookSecondaryBucket::setting bucket "
          "timeout...");
    }
  }
}

bool ClientMetadataService::isBucketMarkedForTimeout(const char* regionFullPath,
                                                     int32_t bucketid) {
  if (m_bucketWaitTimeout == std::chrono::milliseconds::zero()) return false;

  ReadGuard guard(m_PRbucketStatusLock);

  const auto& bs = m_bucketStatus.find(regionFullPath);
  if (bs != m_bucketStatus.end()) {
    bool m = bs->second->isBucketTimedOut(bucketid, m_bucketWaitTimeout);
    if (m) {
      ThinClientPoolDM* tcrdm = dynamic_cast<ThinClientPoolDM*>(m_pool);
      CacheImpl* cache = tcrdm->getConnectionManager().getCacheImpl();
      cache->incBlackListBucketTimeouts();
    }
    LOGFINE("isBucketMarkedForTimeout:: for bucket %d returning = %d", bucketid,
            m);

    return m;
  }

  return false;
}
}  // namespace client
}  // namespace geode
}  // namespace apache
