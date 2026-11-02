// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/metadata_stats.h"

#include <limits>

#include "leveldb/comparator.h"
#include "leveldb/write_batch.h"

namespace leveldb {

bool UserKeyLess::operator()(const std::string& lhs,
                             const std::string& rhs) const {
  return comparator_->Compare(Slice(lhs), Slice(rhs)) < 0;
}

void MetadataDelta::AddRecord(const Slice& user_key) {
  ++total_entries;
  ++key_deltas[user_key.ToString()];
}

Status MetadataDelta::AddRecords(const Slice& user_key, uint64_t count) {
  const int64_t max = std::numeric_limits<int64_t>::max();
  if (count == 0 || count > static_cast<uint64_t>(max) ||
      total_entries > max - static_cast<int64_t>(count)) {
    return Status::Corruption("invalid metadata reference count");
  }
  auto position = key_deltas.find(user_key.ToString());
  if (position != key_deltas.end() &&
      position->second > max - static_cast<int64_t>(count)) {
    return Status::Corruption("metadata key reference count overflow");
  }
  total_entries += static_cast<int64_t>(count);
  key_deltas[user_key.ToString()] += static_cast<int64_t>(count);
  return Status::OK();
}

void MetadataDelta::RemoveRecord(const Slice& user_key) {
  --total_entries;
  --key_deltas[user_key.ToString()];
}

namespace {

class WriteMetadataCollector : public WriteBatch::Handler {
 public:
  explicit WriteMetadataCollector(MetadataDelta* delta) : delta_(delta) {}

  void Put(const Slice& key, const Slice&) override { delta_->AddRecord(key); }
  void Delete(const Slice& key) override { delta_->AddRecord(key); }
  void PutWithTTL(const Slice& key, const Slice&, uint64_t) override {
    // Zero-TTL and expired records still occupy physical storage.
    delta_->AddRecord(key);
  }

 private:
  MetadataDelta* delta_;
};

}  // namespace

Status CollectWriteBatchMetadata(const WriteBatch& batch, MetadataDelta* delta) {
  WriteMetadataCollector collector(delta);
  return batch.Iterate(&collector);
}

static uint64_t NegativeMagnitude(int64_t value) {
  return static_cast<uint64_t>(-(value + 1)) + 1;
}

Status MetadataState::Apply(const MetadataDelta& delta) {
  if (delta.total_entries < 0 &&
      NegativeMagnitude(delta.total_entries) > total_entries_) {
    return Status::Corruption("metadata total_entries underflow");
  }
  if (delta.total_entries > 0 &&
      static_cast<uint64_t>(delta.total_entries) >
          std::numeric_limits<uint64_t>::max() - total_entries_) {
    return Status::Corruption("metadata total_entries overflow");
  }

  // Validate the complete delta first, so a bad compaction delta cannot leave
  // half of the key map updated.
  for (const auto& item : delta.key_deltas) {
    const auto current = key_references_.find(item.first);
    const uint64_t old_count =
        current == key_references_.end() ? 0 : current->second;
    if (item.second < 0 &&
        NegativeMagnitude(item.second) > old_count) {
      return Status::Corruption("metadata key reference underflow", item.first);
    }
    if (item.second > 0 && static_cast<uint64_t>(item.second) >
                               std::numeric_limits<uint64_t>::max() - old_count) {
      return Status::Corruption("metadata key reference overflow", item.first);
    }
  }

  if (delta.total_entries < 0) {
    total_entries_ -= NegativeMagnitude(delta.total_entries);
  } else {
    total_entries_ += static_cast<uint64_t>(delta.total_entries);
  }

  for (const auto& item : delta.key_deltas) {
    auto current = key_references_.find(item.first);
    const uint64_t old_count =
        current == key_references_.end() ? 0 : current->second;
    uint64_t new_count;
    if (item.second < 0) {
      new_count = old_count - NegativeMagnitude(item.second);
    } else {
      new_count = old_count + static_cast<uint64_t>(item.second);
    }

    if (new_count == 0) {
      if (current != key_references_.end()) key_references_.erase(current);
    } else if (current == key_references_.end()) {
      key_references_.emplace(item.first, new_count);
    } else {
      current->second = new_count;
    }
  }
  return Status::OK();
}

void MetadataState::Reset() {
  total_entries_ = 0;
  key_references_.clear();
}

Stats MetadataState::Snapshot(uint64_t sequence) const {
  Stats result;
  result.total_entries = total_entries_;
  result.distinct_keys = key_references_.size();
  result.seq = sequence;
  return result;
}

void SourceMetadataBuilder::Add(const Slice& user_key, bool has_ttl) {
  has_ttl_ = has_ttl_ || has_ttl;
  ++metadata_.total_entries;
  if (!has_previous_key_ ||
      comparator_->Compare(Slice(previous_key_), user_key) != 0) {
    ++metadata_.distinct_keys;
    previous_key_.assign(user_key.data(), user_key.size());
    has_previous_key_ = true;
    references_->emplace_back(previous_key_, 1);
  } else {
    ++references_->back().second;
  }
}

}  // namespace leveldb
