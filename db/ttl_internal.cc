// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/ttl_internal.h"

#include <limits>

#include "leveldb/env.h"
#include "util/coding.h"

namespace leveldb {

Status ComputeExpirationTime(Env* env, uint64_t ttl_seconds,
                             uint64_t* expiration_micros) {
  if (env == nullptr || expiration_micros == nullptr) {
    return Status::InvalidArgument("TTL expiration output or Env is null");
  }
  return ComputeExpirationTimeFromNow(env->NowMicros(), ttl_seconds,
                                      expiration_micros);
}

Status ComputeExpirationTimeFromNow(uint64_t now_micros,
                                    uint64_t ttl_seconds,
                                    uint64_t* expiration_micros) {
  if (expiration_micros == nullptr) {
    return Status::InvalidArgument("TTL expiration output is null");
  }
  if (ttl_seconds >
      std::numeric_limits<uint64_t>::max() / kMicrosPerSecond) {
    return Status::InvalidArgument("TTL seconds overflow microseconds");
  }
  const uint64_t ttl_micros = ttl_seconds * kMicrosPerSecond;
  if (now_micros > std::numeric_limits<uint64_t>::max() - ttl_micros) {
    return Status::InvalidArgument("TTL expiration time overflow");
  }
  *expiration_micros = now_micros + ttl_micros;
  return Status::OK();
}

void EncodeValueWithTTL(uint64_t expiration_micros, const Slice& user_value,
                        std::string* encoded_value) {
  encoded_value->clear();
  PutFixed64(encoded_value, expiration_micros);
  encoded_value->append(user_value.data(), user_value.size());
}

Status DecodeValueWithTTL(const Slice& encoded_value,
                          uint64_t* expiration_micros, Slice* user_value) {
  if (expiration_micros == nullptr || user_value == nullptr) {
    return Status::InvalidArgument("TTL decode output is null");
  }
  if (encoded_value.size() < kExpirationTimeSize) {
    return Status::Corruption("TTL value is missing expiration time");
  }
  *expiration_micros = DecodeFixed64(encoded_value.data());
  *user_value = Slice(encoded_value.data() + kExpirationTimeSize,
                      encoded_value.size() - kExpirationTimeSize);
  return Status::OK();
}

}  // namespace leveldb
