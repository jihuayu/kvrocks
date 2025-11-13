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
 */

#include "common/sha256.h"

#include <array>
#include <cstring>

namespace util {

namespace {

constexpr std::array<uint32_t, 8> kInitialState = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

struct Sha256Ctx {
  std::array<uint32_t, 8> state = kInitialState;
  std::array<uint8_t, 64> buffer{};
  uint64_t bit_len = 0;
  size_t buffer_len = 0;
};

inline uint32_t RotR(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t Sigma0(uint32_t x) { return RotR(x, 2) ^ RotR(x, 13) ^ RotR(x, 22); }
inline uint32_t Sigma1(uint32_t x) { return RotR(x, 6) ^ RotR(x, 11) ^ RotR(x, 25); }
inline uint32_t Gamma0(uint32_t x) { return RotR(x, 7) ^ RotR(x, 18) ^ (x >> 3); }
inline uint32_t Gamma1(uint32_t x) { return RotR(x, 17) ^ RotR(x, 19) ^ (x >> 10); }

void Transform(Sha256Ctx &ctx, const uint8_t data[64]) {
  uint32_t schedule[64];
  for (size_t i = 0; i < 16; ++i) {
    size_t j = i * 4;
    schedule[i] = (static_cast<uint32_t>(data[j]) << 24) | (static_cast<uint32_t>(data[j + 1]) << 16) |
                  (static_cast<uint32_t>(data[j + 2]) << 8) | static_cast<uint32_t>(data[j + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    schedule[i] = Gamma1(schedule[i - 2]) + schedule[i - 7] + Gamma0(schedule[i - 15]) + schedule[i - 16];
  }

  uint32_t a = ctx.state[0];
  uint32_t b = ctx.state[1];
  uint32_t c = ctx.state[2];
  uint32_t d = ctx.state[3];
  uint32_t e = ctx.state[4];
  uint32_t f = ctx.state[5];
  uint32_t g = ctx.state[6];
  uint32_t h = ctx.state[7];

  for (size_t i = 0; i < 64; ++i) {
    uint32_t temp1 = h + Sigma1(e) + Ch(e, f, g) + kRoundConstants[i] + schedule[i];
    uint32_t temp2 = Sigma0(a) + Maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  ctx.state[0] += a;
  ctx.state[1] += b;
  ctx.state[2] += c;
  ctx.state[3] += d;
  ctx.state[4] += e;
  ctx.state[5] += f;
  ctx.state[6] += g;
  ctx.state[7] += h;
}

void Update(Sha256Ctx &ctx, const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    ctx.buffer[ctx.buffer_len++] = data[i];
    if (ctx.buffer_len == ctx.buffer.size()) {
      Transform(ctx, ctx.buffer.data());
      ctx.bit_len += 512;
      ctx.buffer_len = 0;
    }
  }
}

void Finalize(Sha256Ctx &ctx, uint8_t hash[32]) {
  ctx.bit_len += static_cast<uint64_t>(ctx.buffer_len) * 8;

  ctx.buffer[ctx.buffer_len++] = 0x80;
  if (ctx.buffer_len > 56) {
    while (ctx.buffer_len < 64) {
      ctx.buffer[ctx.buffer_len++] = 0;
    }
    Transform(ctx, ctx.buffer.data());
    ctx.buffer_len = 0;
  }

  while (ctx.buffer_len < 56) {
    ctx.buffer[ctx.buffer_len++] = 0;
  }

  for (int i = 7; i >= 0; --i) {
    ctx.buffer[ctx.buffer_len++] = static_cast<uint8_t>((ctx.bit_len >> (i * 8)) & 0xffU);
  }

  Transform(ctx, ctx.buffer.data());

  for (size_t i = 0; i < 8; ++i) {
    hash[i * 4] = static_cast<uint8_t>((ctx.state[i] >> 24) & 0xffU);
    hash[i * 4 + 1] = static_cast<uint8_t>((ctx.state[i] >> 16) & 0xffU);
    hash[i * 4 + 2] = static_cast<uint8_t>((ctx.state[i] >> 8) & 0xffU);
    hash[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i] & 0xffU);
  }
}

}  // namespace

std::string Sha256Hex(std::string_view input) {
  Sha256Ctx ctx;
  ctx.state = kInitialState;
  ctx.bit_len = 0;
  ctx.buffer_len = 0;

  Update(ctx, reinterpret_cast<const uint8_t *>(input.data()), input.size());

  uint8_t digest[32];
  Finalize(ctx, digest);

  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    result[i * 2] = kHex[(digest[i] >> 4) & 0x0f];
    result[i * 2 + 1] = kHex[digest[i] & 0x0f];
  }
  return result;
}

}  // namespace util
