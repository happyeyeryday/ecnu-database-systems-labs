// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/ttl_internal.h"

#include <atomic>
#include <condition_variable>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "db/db_impl.h"
#include "db/db_iter.h"
#include "db/filename.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "leveldb/env.h"
#include "util/coding.h"

namespace leveldb {

TEST(TTLInternalTest, ComputesAbsoluteExpiration) {
  uint64_t expiration = 0;
  ASSERT_TRUE(ComputeExpirationTimeFromNow(123, 2, &expiration).ok());
  ASSERT_EQ(expiration, 2000123);

  ASSERT_TRUE(ComputeExpirationTimeFromNow(123, 0, &expiration).ok());
  ASSERT_EQ(expiration, 123);
  ASSERT_TRUE(IsExpired(expiration, 123));
  ASSERT_FALSE(IsExpired(expiration, 122));
}

TEST(TTLInternalTest, RejectsExpirationOverflow) {
  uint64_t expiration = 0;
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  ASSERT_TRUE(
      ComputeExpirationTimeFromNow(0, max, &expiration).IsInvalidArgument());
  ASSERT_TRUE(
      ComputeExpirationTimeFromNow(max, 1, &expiration).IsInvalidArgument());
  ASSERT_TRUE(
      ComputeExpirationTimeFromNow(0, 1, nullptr).IsInvalidArgument());
}

TEST(TTLInternalTest, EncodesExpirationAndBinaryValue) {
  const std::string original("a\0b", 3);
  std::string encoded;
  EncodeValueWithTTL(987654321, original, &encoded);

  uint64_t expiration = 0;
  Slice decoded;
  ASSERT_TRUE(DecodeValueWithTTL(encoded, &expiration, &decoded).ok());
  ASSERT_EQ(expiration, 987654321);
  ASSERT_EQ(decoded.ToString(), original);

  ASSERT_TRUE(DecodeValueWithTTL(Slice("short"), &expiration, &decoded)
                  .IsCorruption());
}

namespace {

class RecordingHandler : public WriteBatch::Handler {
 public:
  std::string key;
  std::string value;
  void Put(const Slice& k, const Slice& v) override {
    key = k.ToString();
    value = v.ToString();
  }
  void Delete(const Slice&) override {}
};

class TTLRecordingHandler : public RecordingHandler {
 public:
  uint64_t expiration = 0;
  void PutWithTTL(const Slice& k, const Slice& v, uint64_t deadline) override {
    Put(k, v);
    expiration = deadline;
  }
};

class TTLTestEnv : public EnvWrapper {
 public:
  TTLTestEnv() : EnvWrapper(Env::Default()), now(123), clock_reads(0) {}
  uint64_t NowMicros() override {
    clock_reads.fetch_add(1);
    return now.load();
  }
  std::atomic<uint64_t> now;
  std::atomic<uint64_t> clock_reads;
};

class SnapshotGuard {
 public:
  explicit SnapshotGuard(DB* db) : db_(db), snapshot_(db->GetSnapshot()) {}
  ~SnapshotGuard() { db_->ReleaseSnapshot(snapshot_); }
  const Snapshot* get() const { return snapshot_; }

 private:
  DB* db_;
  const Snapshot* snapshot_;
};

// Occupy the default Env's single background worker until this scope ends.
// This lets a read exercise imm_ deterministically, before it is flushed.
class BackgroundBlocker {
 public:
  explicit BackgroundBlocker(Env* env) {
    env->Schedule(&Run, this);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return started_; });
  }

  ~BackgroundBlocker() {
    std::unique_lock<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return finished_; });
  }

 private:
  static void Run(void* arg) {
    auto* blocker = static_cast<BackgroundBlocker*>(arg);
    std::unique_lock<std::mutex> lock(blocker->mutex_);
    blocker->started_ = true;
    blocker->cv_.notify_all();
    blocker->cv_.wait(lock, [blocker] { return blocker->released_; });
    blocker->finished_ = true;
    blocker->cv_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  bool started_ = false;
  bool released_ = false;
  bool finished_ = false;
};

}  // namespace

TEST(TTLWriteBatchTest, RoundTripAndLegacyHandler) {
  WriteBatch batch;
  const std::string binary("a\0b", 3);
  WriteBatchInternal::PutWithTTL(&batch, "key", binary, 2000123);
  ASSERT_EQ(1, WriteBatchInternal::Count(&batch));

  // Simulate serialization into WAL and loading those exact bytes back.
  WriteBatch recovered;
  WriteBatchInternal::SetContents(&recovered,
                                  WriteBatchInternal::Contents(&batch));
  TTLRecordingHandler ttl;
  ASSERT_TRUE(recovered.Iterate(&ttl).ok());
  ASSERT_EQ("key", ttl.key);
  ASSERT_EQ(binary, ttl.value);
  ASSERT_EQ(2000123, ttl.expiration);

  RecordingHandler legacy;
  ASSERT_TRUE(recovered.Iterate(&legacy).ok());
  ASSERT_EQ(binary, legacy.value);
}

TEST(TTLWriteBatchTest, RejectsMalformedRecords) {
  // A length-prefixed value exists, but its expiration header is too short.
  std::string bytes(12, '\0');
  bytes.push_back(static_cast<char>(kTypeValueWithTTL));
  PutLengthPrefixedSlice(&bytes, "key");
  PutLengthPrefixedSlice(&bytes, "short");
  WriteBatch batch;
  WriteBatchInternal::SetContents(&batch, bytes);
  WriteBatchInternal::SetCount(&batch, 1);
  TTLRecordingHandler handler;
  ASSERT_TRUE(batch.Iterate(&handler).IsCorruption());

  WriteBatch valid;
  WriteBatchInternal::PutWithTTL(&valid, "key", "value", 42);
  bytes = WriteBatchInternal::Contents(&valid).ToString();
  bytes.pop_back();
  WriteBatchInternal::SetContents(&batch, bytes);
  ASSERT_TRUE(batch.Iterate(&handler).IsCorruption());

  WriteBatchInternal::SetCount(&valid, 2);
  ASSERT_TRUE(valid.Iterate(&handler).IsCorruption());
}

TEST(TTLWriteBatchTest, MixedBatchPreservesTypesAndSequences) {
  WriteBatch batch, tail;
  batch.Put("a", "plain");
  WriteBatchInternal::PutWithTTL(&tail, "b", "ttl", 42);
  tail.Delete("c");
  batch.Append(tail);
  WriteBatchInternal::SetSequence(&batch, 100);
  ASSERT_EQ(3, WriteBatchInternal::Count(&batch));

  InternalKeyComparator comparator(BytewiseComparator());
  MemTable* mem = new MemTable(comparator);
  mem->Ref();
  const auto unref = [](MemTable* table) { table->Unref(); };
  std::unique_ptr<MemTable, decltype(unref)> owner(mem, unref);
  Status status = WriteBatchInternal::InsertInto(&batch, mem);
  std::unique_ptr<Iterator> iter(mem->NewIterator());
  ASSERT_TRUE(status.ok());
  iter->SeekToFirst();
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(iter->Valid());
    ParsedInternalKey key;
    ASSERT_TRUE(ParseInternalKey(iter->key(), &key));
    ASSERT_EQ(100 + i, key.sequence);
    ASSERT_EQ(i == 0 ? kTypeValue
                    : (i == 1 ? kTypeValueWithTTL : kTypeDeletion), key.type);
    if (i == 1) {
      uint64_t expiration;
      Slice value;
      ASSERT_TRUE(DecodeValueWithTTL(iter->value(), &expiration, &value).ok());
      ASSERT_EQ(42, expiration);
      ASSERT_EQ("ttl", value.ToString());
    }
    iter->Next();
  }
  ASSERT_FALSE(iter->Valid());
  ASSERT_TRUE(iter->status().ok());
}

class TTLWriteRecoveryTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(env_.GetTestDirectory(&path_).ok());
    path_ += "/leveldb_ttl_stage1_" + std::string(
            testing::UnitTest::GetInstance()->current_test_info()->name());
    options_.env = &env_;
    options_.create_if_missing = true;
    options_.reuse_logs = true;
    ASSERT_TRUE(DestroyDB(path_, options_).ok());
    Open();
  }

  void TearDown() override {
    db_.reset();
    EXPECT_TRUE(DestroyDB(path_, options_).ok());
  }

  void Open() {
    DB* db = nullptr;
    ASSERT_TRUE(DB::Open(options_, path_, &db).ok());
    db_.reset(static_cast<DBImpl*>(db));
  }

  void CheckTTL(const std::string& key, uint64_t sequence,
                uint64_t deadline, const std::string& expected) {
    std::unique_ptr<Iterator> iter(db_->TEST_NewInternalIterator());
    InternalKey target(key, kMaxSequenceNumber, kValueTypeForSeek);
    iter->Seek(target.Encode());
    ASSERT_TRUE(iter->Valid());
    ParsedInternalKey parsed;
    ASSERT_TRUE(ParseInternalKey(iter->key(), &parsed));
    ASSERT_EQ(key, parsed.user_key.ToString());
    ASSERT_EQ(sequence, parsed.sequence);
    ASSERT_EQ(kTypeValueWithTTL, parsed.type);
    uint64_t expiration;
    Slice value;
    ASSERT_TRUE(DecodeValueWithTTL(iter->value(), &expiration, &value).ok());
    ASSERT_EQ(deadline, expiration);
    ASSERT_EQ(expected, value.ToString());
    ASSERT_TRUE(iter->status().ok());
  }

  uint64_t LogBytes() {
    std::vector<std::string> files;
    EXPECT_TRUE(env_.GetChildren(path_, &files).ok());
    uint64_t total = 0;
    for (const std::string& file : files) {
      uint64_t number;
      FileType type;
      if (ParseFileName(file, &number, &type) && type == kLogFile) {
        uint64_t size = 0;
        EXPECT_TRUE(env_.GetFileSize(path_ + "/" + file, &size).ok());
        total += size;
      }
    }
    return total;
  }

  TTLTestEnv env_;
  Options options_;
  std::string path_;
  std::unique_ptr<DBImpl> db_;
};

TEST_F(TTLWriteRecoveryTest, ZeroTTLAndOverflowHaveCorrectWriteEffects) {
  WriteOptions write;
  write.sync = true;
  ASSERT_TRUE(db_->Put(write, "zero", "value", 0).ok());
  CheckTTL("zero", 1, 123, "value");
  const uint64_t log_bytes = LogBytes();
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  ASSERT_TRUE(db_->Put(write, "overflow", "bad", max).IsInvalidArgument());
  env_.now.store(max);
  ASSERT_TRUE(db_->Put(write, "overflow", "bad", 1).IsInvalidArgument());
  ASSERT_EQ(log_bytes, LogBytes());
  env_.now.store(456);
  ASSERT_TRUE(db_->Put(write, "next", "value", 1).ok());
  CheckTTL("next", 2, 1000456, "value");

  std::unique_ptr<Iterator> iter(db_->TEST_NewInternalIterator());
  int count = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) ++count;
  ASSERT_TRUE(iter->status().ok());
  ASSERT_EQ(2, count);
}

TEST_F(TTLWriteRecoveryTest, WALReplayPreservesDeadlineEvenAfterExpiry) {
  WriteOptions write;
  write.sync = true;
  const std::string binary("a\0b", 3);
  ASSERT_TRUE(db_->Put(write, "key", binary, 2).ok());
  CheckTTL("key", 1, 2000123, binary);
  std::string sst_count;
  ASSERT_TRUE(db_->GetProperty("leveldb.num-files-at-level0", &sst_count));
  ASSERT_EQ("0", sst_count);
  ASSERT_GT(LogBytes(), 0);
  db_.reset();
  env_.now.store(9000000);
  Open();
  CheckTTL("key", 1, 2000123, binary);
  ASSERT_TRUE(db_->Put(write, "next", "value", 0).ok());
  CheckTTL("next", 2, 9000000, "value");
}

TEST_F(TTLWriteRecoveryTest, FlushAndReopenPreserveDeadline) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "value", 2).ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  CheckTTL("key", 1, 2000123, "value");
  db_.reset();
  env_.now.store(9000000);
  Open();
  CheckTTL("key", 1, 2000123, "value");
}

TEST(TTLReadMemTableTest, HitMissExpiryAndCorruptionAreDistinct) {
  InternalKeyComparator comparator(BytewiseComparator());
  MemTable* mem = new MemTable(comparator);
  mem->Ref();
  const auto unref = [](MemTable* table) { table->Unref(); };
  std::unique_ptr<MemTable, decltype(unref)> owner(mem, unref);
  mem->Add(1, kTypeValue, "key", "old");
  std::string encoded;
  EncodeValueWithTTL(100, "new", &encoded);
  mem->Add(2, kTypeValueWithTTL, "key", encoded);
  mem->Add(3, kTypeValueWithTTL, "broken", "short");

  std::string value;
  Status status;
  ASSERT_FALSE(mem->Get(LookupKey("missing", 3), 100, &value, &status));
  ASSERT_TRUE(mem->Get(LookupKey("key", 3), 99, &value, &status));
  ASSERT_TRUE(status.ok());
  ASSERT_EQ("new", value);
  ASSERT_TRUE(mem->Get(LookupKey("key", 3), 100, &value, &status));
  ASSERT_TRUE(status.IsNotFound());
  status = Status::OK();
  ASSERT_TRUE(mem->Get(LookupKey("key", 1), 100, &value, &status));
  ASSERT_TRUE(status.ok());
  ASSERT_EQ("old", value);
  ASSERT_TRUE(mem->Get(LookupKey("broken", 3), 100, &value, &status));
  ASSERT_TRUE(status.IsCorruption());
}

class TTLReadTest : public TTLWriteRecoveryTest {
 protected:
  void ExpectValue(const Slice& key, const std::string& expected,
                   const Snapshot* snapshot = nullptr) {
    ReadOptions read;
    read.snapshot = snapshot;
    std::string value;
    Status status = db_->Get(read, key, &value);
    ASSERT_TRUE(status.ok()) << status.ToString();
    ASSERT_EQ(expected, value);
  }

  void ExpectMissing(const Slice& key, const Snapshot* snapshot = nullptr) {
    ReadOptions read;
    read.snapshot = snapshot;
    std::string value;
    Status status = db_->Get(read, key, &value);
    ASSERT_TRUE(status.IsNotFound()) << status.ToString();
  }
};

TEST_F(TTLReadTest, MemTableDeadlineBoundaryAndPlainBinaryValues) {
  const std::string binary("a\0b", 3);
  // A plain value with a zero-like prefix must never be interpreted as TTL.
  const std::string plain(16, '\0');
  ASSERT_TRUE(db_->Put(WriteOptions(), "ttl", binary, 2).ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "plain", plain).ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "empty", "", 2).ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "zero", "expired", 0).ok());
  ExpectValue("ttl", binary);
  ExpectValue("empty", "");
  ExpectMissing("zero");
  ExpectMissing("missing");
  env_.now.store(2000122);
  ExpectValue("ttl", binary);
  env_.now.store(2000123);
  ExpectMissing("ttl");
  ExpectMissing("empty");
  env_.now.store(9000000);
  ExpectMissing("ttl");
  ExpectValue("plain", plain);
}

TEST_F(TTLReadTest, SSTDeadlineBoundaryAndRepeatedCacheReads) {
  const std::string binary("a\0b", 3);
  ASSERT_TRUE(db_->Put(WriteOptions(), "ttl", binary, 2).ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "plain", "permanent").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ExpectValue("ttl", binary);
  env_.now.store(2000122);
  ExpectValue("ttl", binary);
  env_.now.store(2000123);
  // A warm table/block cache must not cache the TTL visibility decision.
  ExpectMissing("ttl");
  ExpectValue("plain", "permanent");
}

TEST_F(TTLReadTest, ExpiredMemTableVersionHidesOldSSTValue) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "old").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "new", 1).ok());
  ExpectValue("key", "new");
  env_.now.store(1000123);
  ExpectMissing("key");
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "plain-newest").ok());
  ExpectValue("key", "plain-newest");
  ASSERT_TRUE(db_->Delete(WriteOptions(), "key").ok());
  ExpectMissing("key");
}

TEST_F(TTLReadTest, ImmutableMemTableUsesSameTTLAndSnapshotRules) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "old").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "ttl", 1).ok());
  SnapshotGuard live_view(db_.get());
  BackgroundBlocker blocker(&env_);
  // Exceed the buffer, then trigger rotation with the next ordinary write.
  const std::string filler(options_.write_buffer_size + 1, 'x');
  ASSERT_TRUE(db_->Put(WriteOptions(), "filler", filler).ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "trigger", "rotate").ok());
  ExpectValue("key", "ttl");
  env_.now.store(1000123);
  ExpectMissing("key");
  ExpectValue("key", "ttl", live_view.get());
}

TEST_F(TTLReadTest, ExpiredSSTVersionHidesOlderSSTValue) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "old").ok());
  // Retain the old version even if a compaction occurs while flushing.
  SnapshotGuard old_view(db_.get());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "new", 1).ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ExpectValue("key", "new");
  env_.now.store(1000123);
  ExpectMissing("key");
  ExpectValue("key", "old", old_view.get());
}

TEST_F(TTLReadTest, SnapshotFixesBothSequenceAndTimeAcrossFlush) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "old").ok());
  SnapshotGuard before_write(db_.get());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "ttl", 2).ok());
  env_.now.store(1000000);
  SnapshotGuard before_expiry(db_.get());
  env_.now.store(2000123);
  SnapshotGuard at_expiry(db_.get());
  ExpectValue("key", "old", before_write.get());
  ExpectValue("key", "ttl", before_expiry.get());
  ExpectMissing("key", at_expiry.get());
  ExpectMissing("key");

  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ExpectValue("key", "old", before_write.get());
  ExpectValue("key", "ttl", before_expiry.get());
  ExpectMissing("key", at_expiry.get());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "later").ok());
  ExpectValue("key", "later");
  ExpectValue("key", "ttl", before_expiry.get());
  ExpectMissing("key", at_expiry.get());
}

TEST_F(TTLReadTest, OrdinaryGetSamplesClockOnceAndSnapshotGetNeverSamplesIt) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "value", 2).ok());
  SnapshotGuard view(db_.get());
  for (int storage = 0; storage < 2; ++storage) {
    const uint64_t before = env_.clock_reads.load();
    ExpectValue("key", "value");
    ASSERT_EQ(before + 1, env_.clock_reads.load());
    const uint64_t after = env_.clock_reads.load();
    ExpectValue("key", "value", view.get());
    ASSERT_EQ(after, env_.clock_reads.load());
    if (storage == 0) ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  }
}

TEST_F(TTLReadTest, WALReopenUsesOriginalDeadlineForGet) {
  WriteOptions write;
  write.sync = true;
  ASSERT_TRUE(db_->Put(write, "key", "value", 2).ok());
  db_.reset();
  env_.now.store(1000000);
  Open();
  ExpectValue("key", "value");
  env_.now.store(2000123);
  ExpectMissing("key");
  db_.reset();
  Open();
  ExpectMissing("key");
}

TEST_F(TTLReadTest, SSTReopenUsesOriginalDeadlineForGet) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "value", 2).ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  db_.reset();
  env_.now.store(1000000);
  Open();
  ExpectValue("key", "value");
  env_.now.store(2000123);
  ExpectMissing("key");
  db_.reset();
  Open();
  ExpectMissing("key");
}

class TTLIteratorTest : public TTLReadTest {
 protected:
  using Entries = std::vector<std::pair<std::string, std::string>>;

  void CheckPosition(Iterator* iter, const Entries::value_type& entry) {
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(entry.first, iter->key().ToString());
    ASSERT_EQ(entry.second, iter->value().ToString());
  }

  void CheckView(Iterator* iter, const Entries& expected) {
    iter->SeekToFirst();
    for (const auto& entry : expected) {
      ASSERT_TRUE(iter->Valid());
      CheckPosition(iter, entry);
      iter->Next();
    }
    ASSERT_FALSE(iter->Valid());
    ASSERT_TRUE(iter->status().ok()) << iter->status().ToString();

    iter->SeekToLast();
    for (auto entry = expected.rbegin(); entry != expected.rend(); ++entry) {
      ASSERT_TRUE(iter->Valid());
      CheckPosition(iter, *entry);
      iter->Prev();
    }
    ASSERT_FALSE(iter->Valid());
    ASSERT_TRUE(iter->status().ok()) << iter->status().ToString();

    // Exercise both direction changes at every visible key.
    for (size_t i = 0; i < expected.size(); ++i) {
      iter->Seek(expected[i].first);
      ASSERT_TRUE(iter->Valid());
      CheckPosition(iter, expected[i]);
      iter->Next();
      if (i + 1 < expected.size()) {
        ASSERT_TRUE(iter->Valid());
        CheckPosition(iter, expected[i + 1]);
        iter->Prev();
        ASSERT_TRUE(iter->Valid());
        CheckPosition(iter, expected[i]);
      } else {
        ASSERT_FALSE(iter->Valid());
      }
      iter->Seek(expected[i].first);
      iter->Prev();
      if (i > 0) {
        ASSERT_TRUE(iter->Valid());
        CheckPosition(iter, expected[i - 1]);
        iter->Next();
        ASSERT_TRUE(iter->Valid());
        CheckPosition(iter, expected[i]);
      } else {
        ASSERT_FALSE(iter->Valid());
      }
    }
    iter->Seek("zzzz");
    ASSERT_FALSE(iter->Valid());
    ASSERT_TRUE(iter->status().ok()) << iter->status().ToString();
  }

  void PopulateMixedVersions() {
    ASSERT_TRUE(db_->Put(WriteOptions(), "a", "old").ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "a", "expired", 0).ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "b", "live", 10).ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "c", "old").ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "c", "expired", 1).ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "d", "deleted", 10).ok());
    ASSERT_TRUE(db_->Delete(WriteOptions(), "d").ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "e", "plain").ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "f", "", 10).ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "g", std::string("a\0b", 3), 10).ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "h", "expired", 0).ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "h", "new-plain").ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "z", "expired", 0).ok());
    env_.now.store(1000123);
  }

  Entries MixedExpected() {
    return {{"b", "live"}, {"e", "plain"}, {"f", ""},
            {"g", std::string("a\0b", 3)}, {"h", "new-plain"}};
  }
};

TEST_F(TTLIteratorTest, MemTableSeekTraversalAndDirectionChanges) {
  PopulateMixedVersions();
  std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
  CheckView(iter.get(), MixedExpected());
  iter->Seek("a");
  CheckPosition(iter.get(), {"b", "live"});
  iter->Seek("c");
  CheckPosition(iter.get(), {"e", "plain"});
  iter->Seek("d");
  CheckPosition(iter.get(), {"e", "plain"});
  iter->Seek("z");
  ASSERT_FALSE(iter->Valid());
  ASSERT_TRUE(iter->status().ok());
}

TEST_F(TTLIteratorTest, SSTAndReopenHaveSameView) {
  PopulateMixedVersions();
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  {
    std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
    CheckView(iter.get(), MixedExpected());
  }
  db_.reset();
  Open();
  std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
  CheckView(iter.get(), MixedExpected());
}

TEST_F(TTLIteratorTest, ExpiredImmutableVersionHidesOldSSTInBothDirections) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "plain").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "old").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "z", "plain").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "ttl", 1).ok());
  SnapshotGuard live_view(db_.get());
  BackgroundBlocker blocker(&env_);
  const std::string filler(options_.write_buffer_size + 1, 'x');
  ASSERT_TRUE(db_->Put(WriteOptions(), "filler", filler).ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "trigger", "rotate").ok());
  env_.now.store(1000123);
  std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
  CheckView(iter.get(), {{"a", "plain"}, {"filler", filler},
                          {"trigger", "rotate"}, {"z", "plain"}});
  iter->Seek("key");
  CheckPosition(iter.get(), {"trigger", "rotate"});
  ReadOptions read;
  read.snapshot = live_view.get();
  std::unique_ptr<Iterator> snapshot_iter(db_->NewIterator(read));
  CheckView(snapshot_iter.get(), {{"a", "plain"}, {"key", "ttl"},
                                   {"z", "plain"}});
}

TEST_F(TTLIteratorTest, ImplicitViewIsFixedBeforeFirstSeekAndAcrossCompaction) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "ttl", 1).ok());
  std::unique_ptr<Iterator> mem_view(db_->NewIterator(ReadOptions()));
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  std::unique_ptr<Iterator> sst_view(db_->NewIterator(ReadOptions()));
  // Neither iterator has been positioned yet. Its view is fixed at creation,
  // not at first Seek, and later writes must also be invisible.
  env_.now.store(1000123);
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "new").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "later", "new-key").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  db_->CompactRange(nullptr, nullptr);
  CheckView(mem_view.get(), {{"key", "ttl"}});
  CheckView(sst_view.get(), {{"key", "ttl"}});
  std::unique_ptr<Iterator> fresh(db_->NewIterator(ReadOptions()));
  CheckView(fresh.get(), {{"key", "new"}, {"later", "new-key"}});
}

TEST_F(TTLIteratorTest, SnapshotUsesCreationTimeAndSequenceNotIteratorTime) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "old").ok());
  SnapshotGuard before_write(db_.get());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "ttl", 1).ok());
  env_.now.store(1000122);
  SnapshotGuard before_expiry(db_.get());
  env_.now.store(1000123);
  SnapshotGuard at_expiry(db_.get());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "later").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  env_.now.store(9000000);
  const Snapshot* snapshots[] = {before_write.get(), before_expiry.get(),
                                 at_expiry.get()};
  const Entries expected[] = {{{"key", "old"}}, {{"key", "ttl"}}, {}};
  for (int i = 0; i < 3; ++i) {
    ReadOptions read;
    read.snapshot = snapshots[i];
    std::unique_ptr<Iterator> iter(db_->NewIterator(read));
    CheckView(iter.get(), expected[i]);
  }
}

TEST_F(TTLIteratorTest, EmptyAndAllExpiredAreNormalEndNotErrors) {
  std::unique_ptr<Iterator> empty(db_->NewIterator(ReadOptions()));
  CheckView(empty.get(), {});
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "old").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "expired", 0).ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "b", "expired", 1).ok());
  env_.now.store(1000123);
  std::unique_ptr<Iterator> expired(db_->NewIterator(ReadOptions()));
  CheckView(expired.get(), {});
  expired->Seek("a");
  ASSERT_FALSE(expired->Valid());
  ASSERT_TRUE(expired->status().ok());
}

TEST_F(TTLIteratorTest, TimeIsSampledOnlyAtCreation) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "ttl", 1).ok());
  SnapshotGuard snapshot(db_.get());
  const uint64_t before = env_.clock_reads.load();
  std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
  ASSERT_EQ(before + 1, env_.clock_reads.load());
  env_.now.store(9000000);
  CheckView(iter.get(), {{"key", "ttl"}});
  ASSERT_EQ(before + 1, env_.clock_reads.load());
  ReadOptions read;
  read.snapshot = snapshot.get();
  std::unique_ptr<Iterator> explicit_view(db_->NewIterator(read));
  CheckView(explicit_view.get(), {{"key", "ttl"}});
  ASSERT_EQ(before + 1, env_.clock_reads.load());
  std::unique_ptr<Iterator> fresh(db_->NewIterator(ReadOptions()));
  CheckView(fresh.get(), {});
}

TEST_F(TTLIteratorTest, CorruptTTLStopsBothDirectionsAndErrorRemainsSticky) {
  InternalKeyComparator comparator(BytewiseComparator());
  MemTable* mem = new MemTable(comparator);
  mem->Ref();
  const auto unref = [](MemTable* table) { table->Unref(); };
  std::unique_ptr<MemTable, decltype(unref)> owner(mem, unref);
  std::string encoded;
  EncodeValueWithTTL(1000, "value", &encoded);
  mem->Add(1, kTypeValueWithTTL, "a", encoded);
  mem->Add(2, kTypeValueWithTTL, "b", "short");
  mem->Add(3, kTypeValueWithTTL, "c", encoded);
  std::unique_ptr<Iterator> forward(NewDBIterator(
      db_.get(), BytewiseComparator(), mem->NewIterator(), 3, 100, 1));
  forward->SeekToFirst();
  CheckPosition(forward.get(), {"a", "value"});
  forward->Next();
  ASSERT_FALSE(forward->Valid());
  ASSERT_TRUE(forward->status().IsCorruption());
  forward->Seek("c");
  CheckPosition(forward.get(), {"c", "value"});
  ASSERT_TRUE(forward->status().IsCorruption());
  std::unique_ptr<Iterator> reverse(NewDBIterator(
      db_.get(), BytewiseComparator(), mem->NewIterator(), 3, 100, 1));
  reverse->SeekToLast();
  CheckPosition(reverse.get(), {"c", "value"});
  reverse->Prev();
  ASSERT_FALSE(reverse->Valid());
  ASSERT_TRUE(reverse->status().IsCorruption());
}

TEST_F(TTLIteratorTest, RandomizedViewsMatchIndependentLatestVersionModel) {
  struct Record {
    std::string value;
    bool deleted;
    bool has_ttl;
    uint64_t expiration;
  };
  std::map<std::string, Record> latest;
  std::mt19937 random(2026);
  auto write = [&] {
    for (int i = 0; i < 20; ++i) {
      const std::string key = std::string(1, 'a' + random() % 10);
      const std::string value = std::to_string(random());
      const int op = random() % 3;
      if (op == 0) {
        ASSERT_TRUE(db_->Delete(WriteOptions(), key).ok());
        latest[key] = {"", true, false, 0};
      } else if (op == 1) {
        ASSERT_TRUE(db_->Put(WriteOptions(), key, value).ok());
        latest[key] = {value, false, false, 0};
      } else {
        const uint64_t ttl = random() % 3;
        ASSERT_TRUE(db_->Put(WriteOptions(), key, value, ttl).ok());
        latest[key] = {value, false, true,
                       env_.now.load() + ttl * kMicrosPerSecond};
      }
    }
  };
  auto expected = [&] {
    Entries entries;
    for (const auto& entry : latest) {
      const Record& record = entry.second;
      if (!record.deleted &&
          (!record.has_ttl || env_.now.load() < record.expiration)) {
        entries.emplace_back(entry.first, record.value);
      }
    }
    return entries;
  };
  for (int round = 0; round < 20; ++round) {
    SCOPED_TRACE(round);
    write();
    const Entries saved = expected();
    std::unique_ptr<Iterator> old(db_->NewIterator(ReadOptions()));
    env_.now.fetch_add(kMicrosPerSecond);
    write();
    if (round % 4 == 0) {
      ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
      db_->CompactRange(nullptr, nullptr);
    }
    CheckView(old.get(), saved);
    std::unique_ptr<Iterator> current(db_->NewIterator(ReadOptions()));
    CheckView(current.get(), expected());
  }
}

}  // namespace leveldb
