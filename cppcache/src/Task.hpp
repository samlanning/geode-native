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

#pragma once

#ifndef GEODE_Task_H_
#define GEODE_Task_H_

#include <memory>

#include <ace/Task.h>

#include "AppDomainContext.hpp"
#include "DistributedSystemImpl.hpp"

namespace apache {
namespace geode {
namespace client {
const char NC_thread[] = "NC thread";
template <class T>
class APACHE_GEODE_EXPORT Task : public ACE_Task_Base {
 public:
  /// Handle timeout events.
  typedef int (T::*OPERATION)(volatile bool& isRunning);

  // op_handler is the receiver of the timeout event. timeout is the method to
  // be executed by op_handler_
  Task(T* op_handler, OPERATION op)
      : op_handler_(op_handler),
        m_op(op),
        m_run(false),
        m_threadName(NC_thread),
        m_appDomainContext(createAppDomainContext()) {}

  // op_handler is the receiver of the timeout event. timeout is the method to
  // be executed by op_handler_
  Task(T* op_handler, OPERATION op, const char* tn)
      : op_handler_(op_handler),
        m_op(op),
        m_run(false),
        m_threadName(tn),
        m_appDomainContext(createAppDomainContext()) {}

  ~Task() {}

  void start() {
    m_run = true;
    activate();
  }

  void stop() {
    if (m_run) {
      m_run = false;
      wait();
    }
  }

  void stopNoblock() { m_run = false; }

  int svc(void) {
    DistributedSystemImpl::setThreadName(m_threadName);

    if (m_appDomainContext) {
      int ret;
      m_appDomainContext->run([this, &ret]() {
        ret = (this->op_handler_->*this->m_op)(this->m_run);
      });
      return ret;
    } else {
      return (this->op_handler_->*m_op)(m_run);
    }
  }

 private:
  T* op_handler_;
  /// Handle timeout events.
  OPERATION m_op;
  volatile bool m_run;
  const char* m_threadName;
  std::unique_ptr<AppDomainContext> m_appDomainContext;
};
}  // namespace client
}  // namespace geode
}  // namespace apache

#endif  // GEODE_Task_H_
