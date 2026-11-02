// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/version_edit.h"

#include "gtest/gtest.h"

namespace leveldb {

static void TestEncodeDecode(const VersionEdit& edit) {
  std::string encoded, encoded2;
  edit.EncodeTo(&encoded);
  VersionEdit parsed;
  Status s = parsed.DecodeFrom(encoded);
  ASSERT_TRUE(s.ok()) << s.ToString();
  parsed.EncodeTo(&encoded2);
  ASSERT_EQ(encoded, encoded2);
}

TEST(VersionEditTest, EncodeDecode) {
  static const uint64_t kBig = 1ull << 50;

  VersionEdit edit;
  for (int i = 0; i < 4; i++) {
    TestEncodeDecode(edit);
    edit.AddFile(3, kBig + 300 + i, kBig + 400 + i,
                 InternalKey("foo", kBig + 500 + i, kTypeValue),
                 InternalKey("zoo", kBig + 600 + i, kTypeDeletion));
    edit.RemoveFile(4, kBig + 700 + i);
    edit.SetCompactPointer(i, InternalKey("x", kBig + 900 + i, kTypeValue));
  }

  edit.SetComparatorName("foo");
  edit.SetLogNumber(kBig + 100);
  edit.SetNextFile(kBig + 200);
  edit.SetLastSequence(kBig + 1000);
  TestEncodeDecode(edit);
}

TEST(VersionEditTest, FileMetadataRoundTripAndMixedLegacyRecords) {
  VersionEdit edit;
  const InternalKey first("a", 10, kTypeValue);
  const InternalKey last("z", 1, kTypeDeletion);
  edit.AddFile(1, 100, 4096, first, last, SourceMetadata{123, 12}, true);
  edit.AddFile(2, 101, 8192, first, last);  // Must remain unknown after decode.
  edit.AddFile(3, 102, 16384, first, last, SourceMetadata{999, 50}, true);
  TestEncodeDecode(edit);
  std::string encoded;
  edit.EncodeTo(&encoded);
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(encoded).ok());
  ASSERT_EQ(edit.DebugString(), decoded.DebugString());
  ASSERT_NE(std::string::npos, decoded.DebugString().find("metadata=123/12"));
  ASSERT_NE(std::string::npos, decoded.DebugString().find("metadata=unknown"));
  ASSERT_NE(std::string::npos, decoded.DebugString().find("metadata=999/50"));
}

TEST(VersionEditTest, KnownZeroMetadataIsDifferentFromLegacyUnknown) {
  const InternalKey key("key", 1, kTypeValue);
  VersionEdit legacy, known;
  legacy.AddFile(0, 100, 4096, key, key);
  known.AddFile(0, 100, 4096, key, key, SourceMetadata{}, true);
  std::string old_bytes, new_bytes;
  legacy.EncodeTo(&old_bytes);
  known.EncodeTo(&new_bytes);
  ASSERT_NE(old_bytes, new_bytes);
  ASSERT_EQ(7, static_cast<unsigned char>(old_bytes[0]));
  ASSERT_EQ(10, static_cast<unsigned char>(new_bytes[0]));
  TestEncodeDecode(legacy);
  TestEncodeDecode(known);
}

TEST(VersionEditTest, RejectsTruncatedAndInconsistentFileMetadata) {
  const InternalKey key("key", 1, kTypeValue);
  VersionEdit valid;
  valid.AddFile(0, 100, 4096, key, key, SourceMetadata{3, 2}, true);
  std::string bytes;
  valid.EncodeTo(&bytes);
  bytes.pop_back();
  VersionEdit decoded;
  ASSERT_TRUE(decoded.DecodeFrom(bytes).IsCorruption());
  for (const SourceMetadata metadata : {SourceMetadata{1, 2},
                                         SourceMetadata{1, 0}}) {
    VersionEdit invalid;
    invalid.AddFile(0, 100, 4096, key, key, metadata, true);
    bytes.clear();
    invalid.EncodeTo(&bytes);
    ASSERT_TRUE(decoded.DecodeFrom(bytes).IsCorruption());
  }
}

TEST(VersionEditTest, PersistentKeyReferencesRoundTripAndRejectInvalidCounts) {
  const InternalKey first("a", 3, kTypeValueWithTTL);
  const InternalKey last("b", 1, kTypeDeletion);
  auto references = std::make_shared<FileKeyReferences>();
  references->emplace_back("a", 2);
  references->emplace_back("b", 1);
  VersionEdit edit;
  edit.AddFile(2, 100, 4096, first, last, SourceMetadata{3, 2}, true,
               references, true);
  TestEncodeDecode(edit);
  std::string bytes;
  edit.EncodeTo(&bytes);
  ASSERT_EQ(11, static_cast<unsigned char>(bytes[0]));
  VersionEdit decoded;
  bytes.pop_back();
  ASSERT_TRUE(decoded.DecodeFrom(bytes).IsCorruption());
  for (uint64_t count : {uint64_t{0}, uint64_t{4}}) {
    auto bad = std::make_shared<FileKeyReferences>();
    bad->emplace_back("a", count);
    bad->emplace_back("b", 1);
    VersionEdit invalid;
    invalid.AddFile(2, 100, 4096, first, last, SourceMetadata{3, 2}, true, bad);
    bytes.clear();
    invalid.EncodeTo(&bytes);
    ASSERT_TRUE(decoded.DecodeFrom(bytes).IsCorruption());
  }
}

}  // namespace leveldb
