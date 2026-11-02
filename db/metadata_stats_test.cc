// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/metadata_stats.h"

#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "db/filename.h"
#include "db/log_writer.h"
#include "db/memtable.h"
#include "db/table_cache.h"
#include "db/version_set.h"
#include "db/write_batch_internal.h"
#include "gtest/gtest.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"

namespace leveldb {
namespace {

class FoldComparator : public Comparator {
 public:
  const char* Name() const override { return "test.MetadataFoldComparator"; }
  int Compare(const Slice& a, const Slice& b) const override {
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
      const unsigned char x = Fold(a[i]);
      const unsigned char y = Fold(b[i]);
      if (x != y) return x < y ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
  }
  void FindShortestSeparator(std::string*, const Slice&) const override {}
  void FindShortSuccessor(std::string*) const override {}

 private:
  static unsigned char Fold(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
  }
};

class MetadataTestEnv : public EnvWrapper {
 public:
  MetadataTestEnv() : EnvWrapper(Env::Default()) {}
  uint64_t NowMicros() override { return now.load(); }

  Status NewRandomAccessFile(const std::string& name,
                             RandomAccessFile** result) override {
    uint64_t number;
    FileType type;
    if (ParseFileName(name.substr(name.find_last_of('/') + 1), &number, &type) &&
        type == kTableFile) {
      table_opens.fetch_add(1);
      if (fail_table_open.load()) return Status::IOError("injected SST open");
    }
    return target()->NewRandomAccessFile(name, result);
  }

  Status NewWritableFile(const std::string& name, WritableFile** result) override {
    Status s = target()->NewWritableFile(name, result);
    uint64_t number;
    FileType type;
    const size_t slash = name.find_last_of('/');
    const std::string basename = name.substr(slash + 1);
    if (s.ok() && ParseFileName(basename, &number, &type) &&
        (type == kLogFile || type == kDescriptorFile)) {
      *result = new LogFile(this, *result, type == kLogFile);
    }
    return s;
  }

  void BlockLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_ = true;
    entered_ = false;
  }
  void WaitForLog() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return entered_; });
  }
  void BlockManifest() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_manifest_ = true;
    entered_ = false;
  }
  void ReleaseLog() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_ = false;
    block_manifest_ = false;
    cv_.notify_all();
  }

  std::atomic<uint64_t> now{123};
  std::atomic<bool> fail_append{false};
  std::atomic<bool> fail_sync{false};
  std::atomic<bool> fail_manifest_sync{false};
  std::atomic<bool> fail_table_open{false};
  std::atomic<uint64_t> append_calls{0};
  std::atomic<uint64_t> table_opens{0};

 private:
  class LogFile : public WritableFile {
   public:
    LogFile(MetadataTestEnv* env, WritableFile* file, bool is_log)
        : env_(env), file_(file), is_log_(is_log) {}
    Status Append(const Slice& data) override {
      {
        std::unique_lock<std::mutex> lock(env_->mutex_);
        if ((is_log_ && env_->block_) || (!is_log_ && env_->block_manifest_)) {
          env_->entered_ = true;
          env_->cv_.notify_all();
          env_->cv_.wait(lock, [this] {
            return is_log_ ? !env_->block_ : !env_->block_manifest_;
          });
        }
      }
      if (is_log_) {
        env_->append_calls.fetch_add(1);
        if (env_->fail_append.load()) return Status::IOError("injected WAL append");
      }
      return file_->Append(data);
    }
    Status Close() override { return file_->Close(); }
    Status Flush() override { return file_->Flush(); }
    Status Sync() override {
      if (is_log_ && env_->fail_sync.load()) return Status::IOError("injected WAL sync");
      if (!is_log_ && env_->fail_manifest_sync.load()) {
        return Status::IOError("injected MANIFEST sync");
      }
      return file_->Sync();
    }

   private:
    MetadataTestEnv* env_;
    std::unique_ptr<WritableFile> file_;
    bool is_log_;
  };

  std::mutex mutex_;
  std::condition_variable cv_;
  bool block_ = false;
  bool entered_ = false;
  bool block_manifest_ = false;
};

}  // namespace

TEST(MetadataDeltaTest, CountsEveryOperationIncludingTTLAndDeletion) {
  WriteBatch batch;
  batch.Put("a", "one");
  batch.Put("a", "two");
  batch.Delete("missing");
  WriteBatchInternal::PutWithTTL(&batch, "a", "ttl", 123);
  MetadataDelta delta(BytewiseComparator());
  ASSERT_TRUE(CollectWriteBatchMetadata(batch, &delta).ok());
  ASSERT_EQ(4, delta.total_entries);
  ASSERT_EQ(3, delta.key_deltas.at("a"));
  ASSERT_EQ(1, delta.key_deltas.at("missing"));
  MetadataState state(BytewiseComparator());
  ASSERT_TRUE(state.Apply(delta).ok());
  Stats stats = state.Snapshot(4);
  ASSERT_EQ(4, stats.total_entries);
  ASSERT_EQ(2, stats.distinct_keys);
  ASSERT_EQ(4, stats.seq);
}

TEST(MetadataDeltaTest, ComparatorEquivalentKeysShareOneReferenceCount) {
  FoldComparator comparator;
  MetadataState state(&comparator);
  MetadataDelta delta(&comparator);
  delta.AddRecord("Key");
  delta.AddRecord("KEY");
  ASSERT_TRUE(state.Apply(delta).ok());
  ASSERT_EQ(2, state.Snapshot(2).total_entries);
  ASSERT_EQ(1, state.Snapshot(2).distinct_keys);
  MetadataDelta remove(&comparator);
  remove.RemoveRecord("key");
  ASSERT_TRUE(state.Apply(remove).ok());
  ASSERT_EQ(1, state.Snapshot(2).distinct_keys);
}

TEST(MetadataDeltaTest, RejectsBadDeltaBeforeChangingAnyState) {
  MetadataState state(BytewiseComparator());
  MetadataDelta initial(BytewiseComparator());
  initial.AddRecord("a");
  ASSERT_TRUE(state.Apply(initial).ok());
  MetadataDelta invalid(BytewiseComparator());
  invalid.AddRecord("b");
  invalid.RemoveRecord("missing");
  ASSERT_TRUE(state.Apply(invalid).IsCorruption());
  ASSERT_EQ(1, state.Snapshot(1).total_entries);
  ASSERT_EQ(1, state.Snapshot(1).distinct_keys);
}

class MetadataWriteTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(env_.GetTestDirectory(&path_).ok());
    path_ += "/leveldb_metadata_stage4_" + std::string(
        testing::UnitTest::GetInstance()->current_test_info()->name());
    options_.env = &env_;
    options_.create_if_missing = true;
    // Keep concurrency tests free from physical record reclamation, which is
    // connected in stage six, not in these write-publication tests.
    options_.write_buffer_size = 64 << 20;
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
  void Check(uint64_t total, uint64_t distinct, uint64_t sequence,
             const Snapshot* snapshot = nullptr) {
    ReadOptions read;
    read.snapshot = snapshot;
    Stats stats;
    ASSERT_TRUE(db_->GetCurrentStats(read, &stats).ok());
    ASSERT_EQ(total, stats.total_entries);
    ASSERT_EQ(distinct, stats.distinct_keys);
    ASSERT_EQ(sequence, stats.seq);
  }
  void CheckPhysical(uint64_t expected) {
    std::unique_ptr<Iterator> iter(db_->TEST_NewInternalIterator());
    uint64_t count = 0;
    for (iter->SeekToFirst(); iter->Valid(); iter->Next()) ++count;
    ASSERT_TRUE(iter->status().ok());
    ASSERT_EQ(expected, count);
  }

  FoldComparator comparator_;
  MetadataTestEnv env_;
  Options options_;
  std::string path_;
  std::unique_ptr<DBImpl> db_;
};

TEST_F(MetadataWriteTest, EmptyOverwriteDeleteAndSnapshotIgnored) {
  Check(0, 0, 0);
  ASSERT_TRUE(db_->GetCurrentStats(ReadOptions(), nullptr).IsInvalidArgument());
  const Snapshot* snapshot = db_->GetSnapshot();
  const auto release = [this](const Snapshot* s) { db_->ReleaseSnapshot(s); };
  std::unique_ptr<const Snapshot, decltype(release)> guard(snapshot, release);
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "one").ok());
  Check(1, 1, 1);
  ASSERT_TRUE(db_->Put(WriteOptions(), "b", "one").ok());
  Check(2, 2, 2);
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "two").ok());
  Check(3, 2, 3);
  ASSERT_TRUE(db_->Delete(WriteOptions(), "b").ok());
  Check(4, 2, 4);
  ASSERT_TRUE(db_->Delete(WriteOptions(), "missing").ok());
  Check(5, 3, 5, snapshot);
  CheckPhysical(5);
}

TEST_F(MetadataWriteTest, MixedBatchEmptyBatchAndNullBatch) {
  WriteBatch batch;
  batch.Put("a", "one");
  batch.Put("a", "two");
  batch.Delete("b");
  WriteBatchInternal::PutWithTTL(&batch, "c", "ttl", 123);
  ASSERT_TRUE(db_->Write(WriteOptions(), &batch).ok());
  Check(4, 3, 4);
  WriteBatch empty;
  ASSERT_TRUE(db_->Write(WriteOptions(), &empty).ok());
  Check(4, 3, 4);
  ASSERT_TRUE(db_->Write(WriteOptions(), nullptr).ok());
  Check(4, 3, 4);
  CheckPhysical(4);
}

TEST_F(MetadataWriteTest, TTLExpiryDoesNotChangeCountsAndOverflowDoesNotPublish) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "plain").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "zero", 0).ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "other", "ttl", 1).ok());
  Check(3, 2, 3);
  env_.now.store(1000123);
  std::string value;
  ASSERT_TRUE(db_->Get(ReadOptions(), "key", &value).IsNotFound());
  ASSERT_TRUE(db_->Get(ReadOptions(), "other", &value).IsNotFound());
  Check(3, 2, 3);
  const uint64_t calls = env_.append_calls.load();
  const uint64_t max = std::numeric_limits<uint64_t>::max();
  ASSERT_TRUE(db_->Put(WriteOptions(), "bad", "value", max).IsInvalidArgument());
  env_.now.store(max);
  ASSERT_TRUE(db_->Put(WriteOptions(), "bad", "value", 1).IsInvalidArgument());
  ASSERT_EQ(calls, env_.append_calls.load());
  Check(3, 2, 3);
  CheckPhysical(3);
}

TEST_F(MetadataWriteTest, ComparatorEquivalentAndBinaryKeys) {
  db_.reset();
  ASSERT_TRUE(DestroyDB(path_, options_).ok());
  options_.comparator = &comparator_;
  Open();
  ASSERT_TRUE(db_->Put(WriteOptions(), "Key", "one").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "KEY", "two", 0).ok());
  ASSERT_TRUE(db_->Delete(WriteOptions(), "key").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), std::string("a\0b", 3), "v").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "", "empty-key").ok());
  Check(5, 3, 5);
  CheckPhysical(5);
}

TEST_F(MetadataWriteTest, FlushAndCrossSourceDuplicatesDoNotDoubleCount) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "one").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  Check(1, 1, 1);
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "two").ok());
  ASSERT_TRUE(db_->Delete(WriteOptions(), "other").ok());
  Check(3, 2, 3);
  CheckPhysical(3);
}

TEST_F(MetadataWriteTest, MalformedBatchDoesNotWriteOrPublishAndNextWriteWorks) {
  WriteBatch batch;
  batch.Put("a", "one");
  batch.Put("b", "two");
  std::string bytes = WriteBatchInternal::Contents(&batch).ToString();
  bytes.pop_back();
  WriteBatchInternal::SetContents(&batch, bytes);
  const uint64_t calls = env_.append_calls.load();
  ASSERT_TRUE(db_->Write(WriteOptions(), &batch).IsCorruption());
  ASSERT_EQ(calls, env_.append_calls.load());
  Check(0, 0, 0);
  CheckPhysical(0);
  ASSERT_TRUE(db_->Put(WriteOptions(), "valid", "value").ok());
  Check(1, 1, 1);
}

TEST_F(MetadataWriteTest, WALAppendFailureDoesNotPublishAndDisablesLaterWrites) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "one").ok());
  env_.fail_append.store(true);
  ASSERT_TRUE(db_->Put(WriteOptions(), "b", "two").IsIOError());
  Check(1, 1, 1);
  CheckPhysical(1);
  env_.fail_append.store(false);
  ASSERT_TRUE(db_->Put(WriteOptions(), "c", "three").IsIOError());
  Check(1, 1, 1);
}

TEST_F(MetadataWriteTest, WALSyncFailureDoesNotPublishAndDisablesLaterWrites) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "one").ok());
  env_.fail_sync.store(true);
  WriteOptions write;
  write.sync = true;
  ASSERT_TRUE(db_->Put(write, "b", "two").IsIOError());
  Check(1, 1, 1);
  CheckPhysical(1);
  env_.fail_sync.store(false);
  ASSERT_TRUE(db_->Put(WriteOptions(), "c", "three").IsIOError());
  Check(1, 1, 1);
}

TEST_F(MetadataWriteTest, QueryDuringBlockedWALSeesPreviousWholeBatch) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "initial", "one").ok());
  WriteBatch batch;
  for (int i = 0; i < 100; ++i) batch.Put(std::to_string(i), "value");
  env_.BlockLog();
  Status write_status;
  std::thread writer([&] { write_status = db_->Write(WriteOptions(), &batch); });
  env_.WaitForLog();
  // The writer has released mutex_, but its batch is not published yet.
  Check(1, 1, 1);
  env_.ReleaseLog();
  writer.join();
  ASSERT_TRUE(write_status.ok());
  Check(101, 101, 101);
}

TEST_F(MetadataWriteTest, ConcurrentBatchesAndQueriesNeverExposePartialCounts) {
  constexpr int kThreads = 6;
  constexpr int kBatches = 100;
  constexpr int kRecords = 4;
  std::atomic<int> remaining{kThreads};
  std::atomic<bool> start{false};
  std::vector<std::thread> writers;
  for (int thread = 0; thread < kThreads; ++thread) {
    writers.emplace_back([&, thread] {
      while (!start.load()) std::this_thread::yield();
      for (int i = 0; i < kBatches; ++i) {
        const std::string key = std::to_string(thread) + "/" + std::to_string(i);
        WriteBatch batch;
        batch.Put(key, "one");
        batch.Put(key, "two");
        batch.Delete(key);
        WriteBatchInternal::PutWithTTL(&batch, key, "expired", 123);
        Status s = db_->Write(WriteOptions(), &batch);
        EXPECT_TRUE(s.ok()) << s.ToString();
      }
      remaining.fetch_sub(1);
    });
  }
  start.store(true);
  uint64_t last_sequence = 0;
  do {
    Stats stats;
    Status s = db_->GetCurrentStats(ReadOptions(), &stats);
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(stats.total_entries, stats.seq);
    EXPECT_EQ(0, stats.seq % kRecords);
    EXPECT_EQ(stats.distinct_keys * kRecords, stats.total_entries);
    EXPECT_GE(stats.seq, last_sequence);
    last_sequence = stats.seq;
    std::this_thread::yield();
  } while (remaining.load() != 0);
  for (auto& writer : writers) writer.join();
  Check(kThreads * kBatches * kRecords, kThreads * kBatches,
        kThreads * kBatches * kRecords);
  CheckPhysical(kThreads * kBatches * kRecords);
}

TEST(SourceMetadataTest, SortedBuilderCountsRecordsAndAdjacentDistinctKeys) {
  FoldComparator comparator;
  SourceMetadataBuilder builder(&comparator);
  ASSERT_EQ(0, builder.metadata().total_entries);
  builder.Add("A");
  builder.Add("a");
  builder.Add("b");
  builder.Add("B");
  builder.Add("c");
  ASSERT_EQ(5, builder.metadata().total_entries);
  ASSERT_EQ(3, builder.metadata().distinct_keys);
}

TEST(SourceMetadataTest, MemTableCountsArbitraryOrderAndComparatorEquivalence) {
  FoldComparator user_comparator;
  InternalKeyComparator comparator(&user_comparator);
  MemTable* mem = new MemTable(comparator);
  mem->Ref();
  const auto unref = [](MemTable* table) { table->Unref(); };
  std::unique_ptr<MemTable, decltype(unref)> owner(mem, unref);
  ASSERT_EQ(0, mem->metadata().total_entries);
  ASSERT_EQ(0, mem->metadata().distinct_keys);
  mem->Add(1, kTypeValue, "Z", "one");
  mem->Add(2, kTypeDeletion, "a", "");
  mem->Add(3, kTypeValue, "z", "two");
  mem->Add(4, kTypeValue, "A", "three");
  mem->Add(5, kTypeValue, "", "empty-key");
  ASSERT_EQ(5, mem->metadata().total_entries);
  ASSERT_EQ(3, mem->metadata().distinct_keys);
}

class MetadataFileTest : public MetadataWriteTest {
 protected:
  // Close DB and recover only its MANIFEST. Verify summaries against an
  // independent physical SST scan *after* checking recovery did not open SSTs.
  void ReadFiles(std::vector<FileMetaData>* result) {
    db_.reset();
    InternalKeyComparator comparator(options_.comparator);
    Options internal_options = options_;
    internal_options.comparator = &comparator;
    TableCache cache(path_, internal_options, 10);
    VersionSet versions(path_, &internal_options, &cache, &comparator);
    bool save_manifest = false;
    const uint64_t before = env_.table_opens.load();
    Status s = versions.Recover(&save_manifest);
    ASSERT_TRUE(s.ok()) << s.ToString();
    ASSERT_EQ(before, env_.table_opens.load());
    result->clear();
    for (int level = 0; level < config::kNumLevels; ++level) {
      std::vector<FileMetaData*> files;
      versions.current()->GetOverlappingInputs(level, nullptr, nullptr, &files);
      for (const FileMetaData* file : files) {
        result->push_back(*file);
        if (!file->metadata_known) continue;
        std::unique_ptr<Iterator> iter(
            cache.NewIterator(ReadOptions(), file->number, file->file_size));
        uint64_t total = 0;
        std::set<std::string, UserKeyLess> keys{UserKeyLess(options_.comparator)};
        for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
          ParsedInternalKey key;
          ASSERT_TRUE(ParseInternalKey(iter->key(), &key));
          ++total;
          keys.insert(key.user_key.ToString());
        }
        ASSERT_TRUE(iter->status().ok());
        ASSERT_EQ(total, file->metadata.total_entries);
        ASSERT_EQ(keys.size(), file->metadata.distinct_keys);
      }
    }
    std::sort(result->begin(), result->end(),
              [](const FileMetaData& a, const FileMetaData& b) {
                return a.number < b.number;
              });
  }

  void WriteFiveRecords() {
    ASSERT_TRUE(db_->Put(WriteOptions(), "a", "one").ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "a", "two").ok());
    ASSERT_TRUE(db_->Put(WriteOptions(), "b", "expired", 0).ok());
    ASSERT_TRUE(db_->Delete(WriteOptions(), "b").ok());
    ASSERT_TRUE(db_->Delete(WriteOptions(), "missing").ok());
  }
};

TEST_F(MetadataFileTest, FlushPersistsExactSummaryWithoutChangingGlobalCounts) {
  WriteFiveRecords();
  Check(5, 3, 5);
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  Check(5, 3, 5);
  std::vector<FileMetaData> files;
  ReadFiles(&files);
  ASSERT_EQ(1, files.size());
  ASSERT_TRUE(files[0].metadata_known);
  ASSERT_EQ(5, files[0].metadata.total_entries);
  ASSERT_EQ(3, files[0].metadata.distinct_keys);
}

TEST_F(MetadataFileTest, ManifestRewritesKeepFileMetadataAcrossRepeatedReopen) {
  WriteFiveRecords();
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  std::vector<FileMetaData> initial;
  ReadFiles(&initial);
  ASSERT_EQ(1, initial.size());
  for (int round = 0; round < 3; ++round) {
    std::string before, after;
    ASSERT_TRUE(ReadFileToString(&env_, CurrentFileName(path_), &before).ok());
    Open();  // reuse_logs=false forces a fresh MANIFEST snapshot.
    ASSERT_TRUE(ReadFileToString(&env_, CurrentFileName(path_), &after).ok());
    ASSERT_NE(before, after);
    std::vector<FileMetaData> files;
    ReadFiles(&files);
    ASSERT_EQ(1, files.size());
    ASSERT_EQ(initial[0].number, files[0].number);
    ASSERT_TRUE(files[0].metadata_known);
    ASSERT_EQ(5, files[0].metadata.total_entries);
    ASSERT_EQ(3, files[0].metadata.distinct_keys);
  }
}

TEST_F(MetadataFileTest, WALRecoveryBuildsSSTWithKnownMetadata) {
  WriteOptions write;
  write.sync = true;
  ASSERT_TRUE(db_->Put(write, "a", "one").ok());
  ASSERT_TRUE(db_->Put(write, "a", "two").ok());
  ASSERT_TRUE(db_->Put(write, "b", "expired", 0).ok());
  db_.reset();
  Open();
  std::vector<FileMetaData> files;
  ReadFiles(&files);
  ASSERT_EQ(1, files.size());
  ASSERT_TRUE(files[0].metadata_known);
  ASSERT_EQ(3, files[0].metadata.total_entries);
  ASSERT_EQ(2, files[0].metadata.distinct_keys);
}

TEST_F(MetadataFileTest, CompactionRecomputesEachSplitOutputSummary) {
  options_.compression = kNoCompression;
  options_.max_file_size = 1 << 20;
  // Empty DB: reopening here does not require global statistics rebuilding.
  db_.reset();
  Open();
  const std::string value(16 << 10, 'x');
  for (int version = 0; version < 2; ++version) {
    for (int i = 0; i < 192; ++i) {
      ASSERT_TRUE(db_->Put(WriteOptions(), std::to_string(i), value).ok());
    }
    ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  }
  db_->CompactRange(nullptr, nullptr);
  std::vector<FileMetaData> files;
  ReadFiles(&files);
  ASSERT_GE(files.size(), 2);
  uint64_t total = 0;
  for (const auto& file : files) {
    ASSERT_TRUE(file.metadata_known);
    total += file.metadata.total_entries;
  }
  ASSERT_EQ(192, total);
}

TEST_F(MetadataFileTest, RepairRebuildsAndPersistsFileMetadata) {
  WriteFiveRecords();
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  db_.reset();
  ASSERT_TRUE(RepairDB(path_, options_).ok());
  std::vector<FileMetaData> files;
  ReadFiles(&files);
  ASSERT_EQ(1, files.size());
  ASSERT_TRUE(files[0].metadata_known);
  ASSERT_EQ(5, files[0].metadata.total_entries);
  ASSERT_EQ(3, files[0].metadata.distinct_keys);
}

TEST_F(MetadataFileTest, LegacyUnknownIsUpgradedOnceForGlobalRecovery) {
  WriteFiveRecords();
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  std::vector<FileMetaData> files;
  ReadFiles(&files);
  ASSERT_EQ(1, files.size());
  // Replace the descriptor with a genuine old-format kNewFile record.
  const uint64_t number = files[0].number + 100;
  VersionEdit legacy;
  legacy.SetComparatorName(options_.comparator->Name());
  legacy.SetLogNumber(0);
  legacy.SetNextFile(number + 1);
  legacy.SetLastSequence(5);
  legacy.AddFile(0, files[0].number, files[0].file_size,
                 files[0].smallest, files[0].largest);
  WritableFile* raw = nullptr;
  ASSERT_TRUE(env_.NewWritableFile(DescriptorFileName(path_, number), &raw).ok());
  std::unique_ptr<WritableFile> file(raw);
  log::Writer log(file.get());
  std::string record;
  legacy.EncodeTo(&record);
  ASSERT_TRUE(log.AddRecord(record).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());
  ASSERT_TRUE(SetCurrentFile(&env_, path_, number).ok());
  ReadFiles(&files);
  ASSERT_EQ(1, files.size());
  ASSERT_FALSE(files[0].metadata_known);
  Open();
  Check(5, 3, 5);
  ReadFiles(&files);
  ASSERT_EQ(1, files.size());
  ASSERT_TRUE(files[0].metadata_known);
  ASSERT_NE(nullptr, files[0].key_references);
  env_.fail_table_open.store(true);
  Open();
  Check(5, 3, 5);  // The upgraded MANIFEST suffices without SST access.
  env_.fail_table_open.store(false);
}

class MetadataRecoveryTest : public MetadataFileTest {
 protected:
  void Reopen() {
    db_.reset();
    Open();
  }
  int HighestLevel() {
    int result = 0;
    for (int level = 0; level < config::kNumLevels; ++level) {
      std::string count;
      EXPECT_TRUE(db_->GetProperty("leveldb.num-files-at-level" +
                                  std::to_string(level), &count));
      if (count != "0") result = level;
    }
    return result;
  }
};

TEST_F(MetadataRecoveryTest, EmptyAndMixedWALRecoverExactCountsWithBothLogModes) {
  Reopen();
  Check(0, 0, 0);
  WriteFiveRecords();
  for (bool reuse : {true, false, true}) {
    options_.reuse_logs = reuse;
    Reopen();
    Check(5, 3, 5);
    CheckPhysical(5);
  }
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "later").ok());
  Check(6, 3, 6);
}

TEST_F(MetadataRecoveryTest, SSTSummaryRestoresGlobalMapWithoutOpeningSST) {
  WriteFiveRecords();
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  db_.reset();
  const uint64_t before = env_.table_opens.load();
  env_.fail_table_open.store(true);
  Open();
  Check(5, 3, 5);
  ASSERT_EQ(before, env_.table_opens.load());
  env_.fail_table_open.store(false);
  // The restored per-key references must also support subsequent writes.
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "later").ok());
  Check(6, 3, 6);
}

TEST_F(MetadataRecoveryTest, CrossSSTAndReusedMemTableDeduplicateWithoutWALCopies) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "one").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "two").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "b", "value", 1).ok());
  ASSERT_TRUE(db_->Delete(WriteOptions(), "gone").ok());
  options_.reuse_logs = true;
  Reopen();
  Check(4, 3, 4);
  CheckPhysical(4);
  env_.now.store(1000123);
  std::string value;
  ASSERT_TRUE(db_->Get(ReadOptions(), "b", &value).IsNotFound());
  Check(4, 3, 4);
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "three").ok());
  Check(5, 3, 5);
}

TEST_F(MetadataRecoveryTest, RecoveredComparatorEquivalentReferencesDeduplicate) {
  db_.reset();
  ASSERT_TRUE(DestroyDB(path_, options_).ok());
  options_.comparator = &comparator_;
  Open();
  ASSERT_TRUE(db_->Put(WriteOptions(), "Key", "one").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "KEY", "two").ok());
  ASSERT_TRUE(db_->Delete(WriteOptions(), "key").ok());
  Reopen();
  Check(3, 1, 3);
  CheckPhysical(3);
  db_->CompactRange(nullptr, nullptr);
  // Force the deepest remaining source through a real rewrite as well.
  db_->TEST_CompactRange(HighestLevel(), nullptr, nullptr);
  Check(0, 0, 3);
  CheckPhysical(0);
  Reopen();
  Check(0, 0, 3);
}

TEST_F(MetadataRecoveryTest, CompactionReclaimsExactVersionsAndSurvivesReopen) {
  WriteFiveRecords();
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  db_->CompactRange(nullptr, nullptr);
  Check(1, 1, 5);
  CheckPhysical(1);
  Reopen();
  Check(1, 1, 5);
  ASSERT_TRUE(db_->Delete(WriteOptions(), "a").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  db_->CompactRange(nullptr, nullptr);
  db_->TEST_CompactRange(HighestLevel(), nullptr, nullptr);
  Check(0, 0, 6);
  CheckPhysical(0);
  Reopen();
  Check(0, 0, 6);
}

TEST_F(MetadataRecoveryTest, TTLLastLevelGCProtectsSnapshotAndPinnedIterator) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "key", "value", 1).ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  for (int level = 0; level < config::kNumLevels - 1; ++level) {
    db_->TEST_CompactRange(level, nullptr, nullptr);
  }
  ASSERT_EQ(config::kNumLevels - 1, HighestLevel());
  const Snapshot* snapshot = db_->GetSnapshot();
  std::unique_ptr<Iterator> iter(db_->NewIterator(ReadOptions()));
  env_.now.store(1000123);
  db_->CompactRange(nullptr, nullptr);
  Check(1, 1, 1);
  ReadOptions read;
  read.snapshot = snapshot;
  std::string value;
  ASSERT_TRUE(db_->Get(read, "key", &value).ok());
  ASSERT_EQ("value", value);
  db_->ReleaseSnapshot(snapshot);
  db_->CompactRange(nullptr, nullptr);
  Check(0, 0, 1);
  CheckPhysical(0);
  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ("value", iter->value().ToString());
  ASSERT_TRUE(iter->status().ok());
  iter.reset();
  Reopen();
  Check(0, 0, 1);
}

TEST_F(MetadataRecoveryTest, FailedCompactionInstallDoesNotSubtractItsDelta) {
  WriteFiveRecords();
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  env_.fail_manifest_sync.store(true);
  db_->TEST_CompactRange(HighestLevel(), nullptr, nullptr);
  Check(5, 3, 5);
  CheckPhysical(5);
  env_.fail_manifest_sync.store(false);
  ASSERT_TRUE(db_->Put(WriteOptions(), "later", "value").IsIOError());
  Check(5, 3, 5);
}

TEST_F(MetadataRecoveryTest, QueryDuringCompactionManifestWriteSeesOldWholeState) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "one").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "two").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  const int level = HighestLevel();
  env_.BlockManifest();
  std::thread compact([&] { db_->TEST_CompactRange(level, nullptr, nullptr); });
  env_.WaitForLog();
  Check(2, 1, 2);
  env_.ReleaseLog();
  compact.join();
  Check(1, 1, 2);
  CheckPhysical(1);
}

TEST_F(MetadataRecoveryTest, ConcurrentWritersQueriesAndCompactionAgreeAtEnd) {
  constexpr int kThreads = 4;
  constexpr int kBatches = 80;
  std::atomic<int> remaining{kThreads};
  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  for (int thread = 0; thread < kThreads; ++thread) {
    writers.emplace_back([&, thread] {
      for (int i = 0; i < kBatches; ++i) {
        const std::string key = std::to_string(thread) + "/" + std::to_string(i);
        WriteBatch batch;
        batch.Put(key, "old");
        batch.Put(key, "new");
        Status s = db_->Write(WriteOptions(), &batch);
        EXPECT_TRUE(s.ok());
      }
      remaining.fetch_sub(1);
    });
  }
  std::thread compactor([&] {
    while (!stop.load()) {
      db_->CompactRange(nullptr, nullptr);
      std::this_thread::yield();
    }
  });
  uint64_t last = 0;
  do {
    Stats stats;
    EXPECT_TRUE(db_->GetCurrentStats(ReadOptions(), &stats).ok());
    EXPECT_EQ(0, stats.seq % 2);
    EXPECT_EQ(stats.seq, stats.distinct_keys * 2);
    EXPECT_GE(stats.total_entries, stats.distinct_keys);
    EXPECT_LE(stats.total_entries, stats.seq);
    EXPECT_GE(stats.seq, last);
    last = stats.seq;
    std::this_thread::yield();
  } while (remaining.load() != 0);
  for (auto& writer : writers) writer.join();
  stop.store(true);
  compactor.join();
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  for (int level = 0; level < config::kNumLevels; ++level) {
    db_->TEST_CompactRange(level, nullptr, nullptr);
  }
  Check(kThreads * kBatches, kThreads * kBatches, 2 * kThreads * kBatches);
  CheckPhysical(kThreads * kBatches);
  Reopen();
  Check(kThreads * kBatches, kThreads * kBatches, 2 * kThreads * kBatches);
}

TEST_F(MetadataRecoveryTest, InvalidPersistedReferenceOrderMakesOpenFail) {
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "one").ok());
  ASSERT_TRUE(db_->Put(WriteOptions(), "b", "two").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  std::vector<FileMetaData> files;
  ReadFiles(&files);
  ASSERT_EQ(1, files.size());
  auto references = std::make_shared<FileKeyReferences>();
  references->emplace_back("b", 1);
  references->emplace_back("a", 1);
  VersionEdit bad;
  const uint64_t number = files[0].number + 100;
  bad.SetComparatorName(options_.comparator->Name());
  bad.SetLogNumber(0);
  bad.SetNextFile(number + 1);
  bad.SetLastSequence(2);
  bad.AddFile(0, files[0].number, files[0].file_size, files[0].smallest,
              files[0].largest, SourceMetadata{2, 2}, true, references);
  WritableFile* raw_file = nullptr;
  ASSERT_TRUE(env_.NewWritableFile(DescriptorFileName(path_, number), &raw_file).ok());
  std::unique_ptr<WritableFile> file(raw_file);
  log::Writer log(file.get());
  std::string record;
  bad.EncodeTo(&record);
  ASSERT_TRUE(log.AddRecord(record).ok());
  ASSERT_TRUE(file->Sync().ok());
  ASSERT_TRUE(file->Close().ok());
  ASSERT_TRUE(SetCurrentFile(&env_, path_, number).ok());
  DB* raw_db = nullptr;
  Status s = DB::Open(options_, path_, &raw_db);
  std::unique_ptr<DB> owner(raw_db);
  ASSERT_TRUE(s.IsCorruption()) << s.ToString();
  ASSERT_EQ(nullptr, raw_db);
}

#if GTEST_HAS_DEATH_TEST
TEST_F(MetadataRecoveryTest, SyncedWritesSurviveExitWithoutDBDestruction) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  ASSERT_TRUE(db_->Put(WriteOptions(), "a", "sst").ok());
  ASSERT_TRUE(db_->TEST_CompactMemTable().ok());
  db_.reset();
  ASSERT_EXIT({
    DB* raw = nullptr;
    if (!DB::Open(options_, path_, &raw).ok()) std::_Exit(10);
    WriteOptions write;
    write.sync = true;
    if (!raw->Put(write, "a", "wal").ok() ||
        !raw->Put(write, "b", "ttl", 1).ok() ||
        !raw->Delete(write, "gone").ok()) std::_Exit(11);
    WriteBatch batch;
    batch.Put("batch", "one");
    batch.Delete("batch");
    if (!raw->Write(write, &batch).ok()) std::_Exit(12);
    std::_Exit(0);  // No Close, destructors, or clean-shutdown metadata save.
  }, testing::ExitedWithCode(0), "");
  options_.reuse_logs = true;
  Open();
  Check(6, 4, 6);
  CheckPhysical(6);
  std::string value;
  ASSERT_TRUE(db_->Get(ReadOptions(), "b", &value).ok());
  env_.now.store(1000123);
  ASSERT_TRUE(db_->Get(ReadOptions(), "b", &value).IsNotFound());
  Check(6, 4, 6);
}
#endif

}  // namespace leveldb
