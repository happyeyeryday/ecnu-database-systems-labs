// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_TTL_INTERNAL_H_
#define STORAGE_LEVELDB_DB_TTL_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class Env;

constexpr uint64_t kMicrosPerSecond = 1000000;
constexpr size_t kExpirationTimeSize = sizeof(uint64_t);

// Calculates one absolute deadline before a TTL write enters DBImpl::Write.
// On overflow, the caller must return this error without allocating a sequence.
Status ComputeExpirationTime(Env* env, uint64_t ttl_seconds,
                             uint64_t* expiration_micros);
Status ComputeExpirationTimeFromNow(uint64_t now_micros,
                                    uint64_t ttl_seconds,
                                    uint64_t* expiration_micros);

// Persistent kTypeValueWithTTL payload:
//   fixed64 expiration_micros | unmodified user value
void EncodeValueWithTTL(uint64_t expiration_micros, const Slice& user_value,
                        std::string* encoded_value);

// The returned user_value aliases encoded_value.
Status DecodeValueWithTTL(const Slice& encoded_value,
                          uint64_t* expiration_micros, Slice* user_value);

inline bool IsExpired(uint64_t expiration_micros,
                      uint64_t read_time_micros) {
  return read_time_micros >= expiration_micros;
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_TTL_INTERNAL_H_
