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

#include "ThinClientBaseDM.hpp"

#include <chrono>

#include <geode/AuthenticatedView.hpp>

#include "ThinClientRegion.hpp"
#include "UserAttributes.hpp"

namespace apache {
namespace geode {
namespace client {

volatile bool ThinClientBaseDM::s_isDeltaEnabledOnServer = true;
const char* ThinClientBaseDM::NC_ProcessChunk = "NC ProcessChunk";

ThinClientBaseDM::ThinClientBaseDM(TcrConnectionManager& connManager,
                                   ThinClientRegion* theRegion)
    : m_region(theRegion),
      m_connManager(connManager),
      m_initDone(false),
      m_clientNotification(false),
      m_chunks(true),
      m_chunkProcessor(nullptr) {}

ThinClientBaseDM::~ThinClientBaseDM() = default;

void ThinClientBaseDM::init() {
  const auto& systemProperties = m_connManager.getCacheImpl()
                                     ->getDistributedSystem()
                                     .getSystemProperties();

  if (!systemProperties.isGridClient() &&
      systemProperties.enableChunkHandlerThread()) {
    startChunkProcessor();
  }

  m_initDone = true;
}

bool ThinClientBaseDM::isSecurityOn() {
  return m_connManager.getCacheImpl()->getAuthInitialize() != nullptr;
}

void ThinClientBaseDM::destroy(bool) {
  if (!m_initDone) {
    // nothing to be done
    return;
  }
  // stop the chunk processing thread
  stopChunkProcessor();
  m_initDone = false;
}

GfErrType ThinClientBaseDM::sendSyncRequestRegisterInterest(
    TcrMessage& request, TcrMessageReply& reply, bool attemptFailover,
    ThinClientRegion*, TcrEndpoint* endpoint) {
  GfErrType err = GF_NOERR;

  if (endpoint == nullptr) {
    err = sendSyncRequest(request, reply, attemptFailover);
  } else {
    reply.setDM(this);
    if (endpoint->connected()) {
      err = sendRequestToEP(request, reply, endpoint);
    } else {
      err = GF_NOTCON;
    }
  }

  if (err == GF_NOERR) {
    switch (reply.getMessageType()) {
      case TcrMessage::REPLY:
      case TcrMessage::RESPONSE:
      case TcrMessage::RESPONSE_FROM_PRIMARY:
      case TcrMessage::RESPONSE_FROM_SECONDARY:
        break;

      case TcrMessage::EXCEPTION:
        err = ThinClientRegion::handleServerException("registerInterest",
                                                      reply.getException());
        break;

      case TcrMessage::REGISTER_INTEREST_DATA_ERROR:
        err = GF_CACHESERVER_EXCEPTION;
        break;

      case TcrMessage::UNREGISTER_INTEREST_DATA_ERROR:
        err = GF_CACHESERVER_EXCEPTION;
        break;

      default:
        LOGERROR(
            "Unknown message type %d during register subscription interest",
            reply.getMessageType());
        err = GF_MSG;
        break;
    }
  }

  // top level should only see NotConnectedException
  if (err == GF_IOERR) {
    err = GF_NOTCON;
  }
  return err;
}

GfErrType ThinClientBaseDM::handleEPError(TcrEndpoint* ep,
                                          TcrMessageReply& reply,
                                          GfErrType error) {
  if (error == GF_NOERR) {
    if (reply.getMessageType() == TcrMessage::EXCEPTION) {
      const char* exceptStr = reply.getException();
      if (exceptStr != nullptr) {
        bool markServerDead = unrecoverableServerError(exceptStr);
        bool doFailover = (markServerDead || nonFatalServerError(exceptStr));
        if (doFailover) {
          LOGFINE(
              "ThinClientDistributionManager::sendRequestToEP: retrying for "
              "server [%s] exception: %s",
              ep->name().c_str(), exceptStr);
          error = GF_NOTCON;
          if (markServerDead) {
            ep->setConnectionStatus(false);
          }
        }
      }
    }
  }
  return error;
}

GfErrType ThinClientBaseDM::sendRequestToEndPoint(const TcrMessage& request,
                                                  TcrMessageReply& reply,
                                                  TcrEndpoint* ep) {
  GfErrType error = GF_NOERR;
  LOGDEBUG("ThinClientBaseDM::sendRequestToEP: invoking endpoint send for: %s",
           ep->name().c_str());
  error = ep->send(request, reply);
  LOGDEBUG(
      "ThinClientBaseDM::sendRequestToEP: completed endpoint send for: %s "
      "[error:%d]",
      ep->name().c_str(), error);
  return handleEPError(ep, reply, error);
}

/**
 * If we receive an exception back from the server, we should retry on
 * other servers for some exceptions. Some exceptions indicate the server
 * is no longer usable while others indicate a temporary condition on
 * the server so that need not be marked as dead.
 * This method is for exceptions when server should be marked as dead.
 */
bool ThinClientBaseDM::unrecoverableServerError(const char* exceptStr) {
  return (
      (strstr(exceptStr, "org.apache.geode.cache.CacheClosedException") !=
       nullptr) ||
      (strstr(exceptStr, "org.apache.geode.distributed.ShutdownException") !=
       nullptr) ||
      (strstr(exceptStr, "java.lang.OutOfMemoryError") != nullptr));
}

/**
 * If we receive an exception back from the server, we should retry on
 * other servers for some exceptions. Some exceptions indicate the server
 * is no longer usable while others indicate a temporary condition on
 * the server so that need not be marked as dead.
 * This method is for exceptions when server should *not* be marked as dead.
 */
bool ThinClientBaseDM::nonFatalServerError(const char* exceptStr) {
  return ((strstr(exceptStr, "org.apache.geode.distributed.TimeoutException") !=
           nullptr) ||
          (strstr(exceptStr, "org.apache.geode.ThreadInterruptedException") !=
           nullptr) ||
          (strstr(exceptStr, "java.lang.IllegalStateException") != nullptr));
}

void ThinClientBaseDM::failover() {}

void ThinClientBaseDM::queueChunk(TcrChunkedContext* chunk) {
  LOGDEBUG("ThinClientBaseDM::queueChunk");
  if (m_chunkProcessor == nullptr) {
    LOGDEBUG("ThinClientBaseDM::queueChunk2");
    // process in same thread if no chunk processor thread
    chunk->handleChunk(true);
    _GEODE_SAFE_DELETE(chunk);
  } else if (!m_chunks.putFor(chunk, std::chrono::seconds(1))) {
    LOGDEBUG("ThinClientBaseDM::queueChunk3");
    // if put in queue fails due to whatever reason then process in same thread
    LOGFINE(
        "addChunkToQueue: timed out while adding to queue of "
        "unbounded size after waiting for 1 secs");
    chunk->handleChunk(true);
    _GEODE_SAFE_DELETE(chunk);
  } else {
    LOGDEBUG("Adding message to ThinClientBaseDM::queueChunk");
  }
}

// the chunk processing thread
int ThinClientBaseDM::processChunks(volatile bool& isRunning) {
  TcrChunkedContext* chunk;
  LOGFINE("Starting chunk process thread for region %s",
          (m_region != nullptr ? m_region->getFullPath().c_str() : "(null)"));
  while (isRunning) {
    chunk = m_chunks.getFor(std::chrono::microseconds(100000));
    if (chunk) {
      chunk->handleChunk(false);
      _GEODE_SAFE_DELETE(chunk);
    }
  }
  LOGFINE("Ending chunk process thread for region %s",
          (m_region != nullptr ? m_region->getFullPath().c_str() : "(null)"));
  GF_DEV_ASSERT(m_chunks.size() == 0);
  return 0;
}

// start the chunk processing thread
void ThinClientBaseDM::startChunkProcessor() {
  if (m_chunkProcessor == nullptr) {
    m_chunks.open();
    m_chunkProcessor = new Task<ThinClientBaseDM>(
        this, &ThinClientBaseDM::processChunks, NC_ProcessChunk);
    m_chunkProcessor->start();
  }
}

// stop the chunk processing thread
void ThinClientBaseDM::stopChunkProcessor() {
  if (m_chunkProcessor != nullptr) {
    m_chunkProcessor->stop();
    m_chunks.close();
    _GEODE_SAFE_DELETE(m_chunkProcessor);
  }
}

void ThinClientBaseDM::beforeSendingRequest(const TcrMessage& request,
                                            TcrConnection* conn) {
  LOGDEBUG(
      "ThinClientBaseDM::beforeSendingRequest %d  "
      "TcrMessage::isUserInitiativeOps(request) = %d ",
      request.isMetaRegion(), TcrMessage::isUserInitiativeOps(request));
  LOGDEBUG(
      "ThinClientBaseDM::beforeSendingRequest %d this->isMultiUserMode() = %d "
      "messageType = %d ",
      this->isSecurityOn(), this->isMultiUserMode(), request.getMessageType());
  if (!(request.isMetaRegion()) && TcrMessage::isUserInitiativeOps(request) &&
      (this->isSecurityOn() || this->isMultiUserMode())) {
    int64_t connId = 0;
    int64_t uniqueId = 0;

    if (!this->isMultiUserMode()) {
      connId = conn->getConnectionId();
      uniqueId = conn->getEndpointObject()->getUniqueId();
    } else {
      connId = conn->getConnectionId();
      if (!(request.getMessageType() == TcrMessage::USER_CREDENTIAL_MESSAGE)) {
        uniqueId = UserAttributes::threadLocalUserAttributes
                       ->getConnectionAttribute(conn->getEndpointObject())
                       ->getUniqueId();
      }
    }

    if (request.getMessageType() == TcrMessage::USER_CREDENTIAL_MESSAGE) {
      auto* req = const_cast<TcrMessage*>(&request);
      req->createUserCredentialMessage(conn);
      req->addSecurityPart(connId, conn);
    } else if (TcrMessage::isUserInitiativeOps(request)) {
      auto* req = const_cast<TcrMessage*>(&request);
      req->addSecurityPart(connId, uniqueId, conn);
    }
  }
}

void ThinClientBaseDM::afterSendingRequest(const TcrMessage& request,
                                           TcrMessageReply& reply,
                                           TcrConnection* conn) {
  LOGDEBUG("ThinClientBaseDM::afterSendingRequest reply msgtype = %d ",
           reply.getMessageType());
  if (!reply.isMetaRegion() && TcrMessage::isUserInitiativeOps(request) &&
      (this->isSecurityOn() || this->isMultiUserMode())) {
    // need to handle encryption/decryption
    if (request.getMessageType() == TcrMessage::USER_CREDENTIAL_MESSAGE) {
      if (TcrMessage::RESPONSE == reply.getMessageType()) {
        if (this->isMultiUserMode()) {
          UserAttributes::threadLocalUserAttributes->setConnectionAttributes(
              conn->getEndpointObject(), reply.getUniqueId(conn));
        } else {
          conn->getEndpointObject()->setUniqueId(reply.getUniqueId(conn));
        }
      }
      conn->setConnectionId(reply.getConnectionId(conn));
    } else if (TcrMessage::isUserInitiativeOps(request)) {
      // bugfix: if noack op then reuse previous security token.
      conn->setConnectionId(reply.getMessageType() == TcrMessage::INVALID
                                ? conn->getConnectionId()
                                : reply.getConnectionId(conn));
    }
  }
}

GfErrType ThinClientBaseDM::sendSyncRequestRegisterInterestEP(TcrMessage&,
                                                              TcrMessageReply&,
                                                              bool,
                                                              TcrEndpoint*) {
  return GF_NOERR;
}

GfErrType ThinClientBaseDM::registerInterestForRegion(TcrEndpoint*,
                                                      const TcrMessage*,
                                                      TcrMessageReply*) {
  return GF_NOERR;
}

bool ThinClientBaseDM::isEndpointAttached(TcrEndpoint*) { return false; }

}  // namespace client
}  // namespace geode
}  // namespace apache
