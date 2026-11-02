// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_METADATA_STATS_H_
#define STORAGE_LEVELDB_DB_METADATA_STATS_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "leveldb/db.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class Comparator;
class WriteBatch;

// Uses the configured LevelDB comparator so comparator-equivalent byte strings
// are counted as one user key.
class UserKeyLess {
 public:
  explicit UserKeyLess(const Comparator* comparator) : comparator_(comparator) {}

  bool operator()(const std::string& lhs, const std::string& rhs) const;

 private:
  const Comparator* comparator_;
};

using KeyReferenceCounts = std::map<std::string, uint64_t, UserKeyLess>;
using FileKeyReferences = std::vector<std::pair<std::string, uint64_t>>;

// A transaction-sized change. Build this while doing write/compaction work,
// then apply it once at the publication point while DBImpl::mutex_ is held.
class MetadataDelta {
 public:
  explicit MetadataDelta(const Comparator* comparator)
      : total_entries(0), key_deltas(UserKeyLess(comparator)) {}

  void AddRecord(const Slice& user_key);
  Status AddRecords(const Slice& user_key, uint64_t count);
  void RemoveRecord(const Slice& user_key);
  bool empty() const { return total_entries == 0 && key_deltas.empty(); }

  int64_t total_entries;
  std::map<std::string, int64_t, UserKeyLess> key_deltas;
};

// Collect every internal record in a complete (possibly grouped) batch.
// The caller must discard the delta if parsing fails.
Status CollectWriteBatchMetadata(const WriteBatch& batch, MetadataDelta* delta);

// Exact global metadata for all internal records in mem_, imm_, and the
// current Version. This class has no lock: DBImpl must protect every call with
// the same mutex that publishes LastSequence and Version changes.
class MetadataState {
 public:
  explicit MetadataState(const Comparator* comparator)
      : total_entries_(0), key_references_(UserKeyLess(comparator)) {}

  Status Apply(const MetadataDelta& delta);
  void Reset();
  Stats Snapshot(uint64_t sequence) const;

 private:
  uint64_t total_entries_;
  KeyReferenceCounts key_references_;
};

// Small per-MemTable/per-SST summary. distinct_keys cannot be summed across
// sources; global de-duplication is provided by MetadataState instead.
struct SourceMetadata {
  uint64_t total_entries = 0;
  uint64_t distinct_keys = 0;
};

// Internal records arrive in user-key order when building an SST.
class SourceMetadataBuilder {
 public:
  explicit SourceMetadataBuilder(const Comparator* comparator)
      : comparator_(comparator), references_(std::make_shared<FileKeyReferences>()) {}

  void Add(const Slice& user_key, bool has_ttl = false);
  const SourceMetadata& metadata() const { return metadata_; }
  std::shared_ptr<const FileKeyReferences> references() const { return references_; }
  bool has_ttl() const { return has_ttl_; }

 private:
  const Comparator* comparator_;
  SourceMetadata metadata_;
  std::string previous_key_;
  bool has_previous_key_ = false;
  std::shared_ptr<FileKeyReferences> references_;
  bool has_ttl_ = false;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_METADATA_STATS_H_
