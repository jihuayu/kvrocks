/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "status.h"

// Flags for notify-keyspace-events, separate from RedisType.
enum NotifyKeyspaceEventFlag {
  kNotifyKeyspace = 1 << 0,  // K, keyspace channels
  kNotifyKeyevent = 1 << 1,  // E, keyevent channels
  kNotifyGeneric = 1 << 2,   // g, emits del
  kNotifyString = 1 << 3,    // $, emits set
  // A, supported data classes without K or E.
  kNotifyAll = kNotifyGeneric | kNotifyString,
};

bool ShouldNotifyKeyspaceEvent(int notify_flags, int type_flag);

struct KeyspaceEvent {
  int type_flag;
  std::string event;
  std::string ns;
  std::string key;
};

// Request-scoped journal for semantic-layer keyspace events.
// Connection owns publish timing and lifetime.
class KeyspaceEventJournal {
 public:
  KeyspaceEventJournal(std::string ns, int notify_flags)
      : ns_(std::move(ns)), notify_flags_(notify_flags) {}

  bool IsEnabled(int type_flag) const { return ShouldNotifyKeyspaceEvent(notify_flags_, type_flag); }

  void Add(int type_flag, std::string_view event, std::string_view key) {
    if (!IsEnabled(type_flag)) return;
    events_.emplace_back(KeyspaceEvent{type_flag, std::string(event), ns_, std::string(key)});
  }

  size_t Mark() const { return events_.size(); }

  void Rollback(size_t mark) { events_.resize(mark); }

  std::vector<KeyspaceEvent> TakeFrom(size_t mark) {
    std::vector<KeyspaceEvent> result;
    result.reserve(events_.size() - mark);
    for (size_t i = mark; i < events_.size(); ++i) {
      result.emplace_back(std::move(events_[i]));
    }
    events_.resize(mark);
    return result;
  }

 private:
  std::string ns_;
  int notify_flags_ = 0;
  std::vector<KeyspaceEvent> events_;
};

// Rolls back journal events on failure unless Commit() is called.
class KeyspaceEventScope {
 public:
  explicit KeyspaceEventScope(KeyspaceEventJournal *journal) : journal_(journal), mark_(journal ? journal->Mark() : 0) {}

  ~KeyspaceEventScope() {
    if (journal_ != nullptr && !committed_) {
      journal_->Rollback(mark_);
    }
  }

  std::vector<KeyspaceEvent> Commit() {
    committed_ = true;
    if (journal_ == nullptr) {
      return {};
    }
    return journal_->TakeFrom(mark_);
  }

 private:
  KeyspaceEventJournal *journal_;
  size_t mark_;
  bool committed_ = false;
};

// Parses notify-keyspace-events flags.
Status ParseNotifyKeyspaceEventsFlags(const std::string &input, int *flags);

// Maps namespaces to notification db names.
// Default namespace maps to 0; database namespaces map back to db indexes when redis-databases is enabled.
// Other namespaces map to ns:encoded-name.
std::string MapNamespaceToKeyspaceDB(const std::string &ns, int redis_databases);
