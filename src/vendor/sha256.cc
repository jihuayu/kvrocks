/*
SHA-256 implementation (public domain)

This file contains a minimal SHA-256 implementation derived from
widely used public-domain sources (e.g. Brad Conte's crypto-algorithms).
To the extent possible under law, the authors have dedicated all copyright
and related and neighboring rights to the public domain. You can copy,
modify, distribute and perform the work, even for commercial purposes,
all without asking permission.

THIS SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
*/

#include "sha256.h"

#include <cstring>

namespace {
// NOLINTNEXTLINE
inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32U - n)); }
// NOLINTNEXTLINE
inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
// NOLINTNEXTLINE
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
// NOLINTNEXTLINE
inline uint32_t ep0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
// NOLINTNEXTLINE
inline uint32_t ep1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
// NOLINTNEXTLINE
inline uint32_t sig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
// NOLINTNEXTLINE
inline uint32_t sig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

// NOLINTNEXTLINE
void transform(SHA256_CTX *ctx, const unsigned char data[64]) {
  uint32_t m[64];
  for (unsigned i = 0; i < 16; ++i) {
    unsigned j = i * 4;
    m[i] = (static_cast<uint32_t>(data[j]) << 24) | (static_cast<uint32_t>(data[j + 1]) << 16) |
           (static_cast<uint32_t>(data[j + 2]) << 8) | static_cast<uint32_t>(data[j + 3]);
  }
  for (unsigned i = 16; i < 64; ++i) {
    m[i] = sig1(m[i - 2]) + m[i - 7] + sig0(m[i - 15]) + m[i - 16];
  }

  uint32_t a = ctx->state[0];
  uint32_t b = ctx->state[1];
  uint32_t c = ctx->state[2];
  uint32_t d = ctx->state[3];
  uint32_t e = ctx->state[4];
  uint32_t f = ctx->state[5];
  uint32_t g = ctx->state[6];
  uint32_t h = ctx->state[7];

  for (unsigned i = 0; i < 64; ++i) {
    uint32_t t1 = h + ep1(e) + ch(e, f, g) + K[i] + m[i];
    uint32_t t2 = ep0(a) + maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

}  // namespace

void SHA256_Init(SHA256_CTX *ctx) {
  static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::memcpy(ctx->state, H0, sizeof(H0));
  ctx->bitlen = 0;
  ctx->datalen = 0;
}

void SHA256_Update(SHA256_CTX *ctx, const unsigned char *data, std::size_t len) {
  for (std::size_t i = 0; i < len; ++i) {
    ctx->data[ctx->datalen++] = data[i];
    if (ctx->datalen == 64) {
      transform(ctx, ctx->data);
      ctx->bitlen += 512;
      ctx->datalen = 0;
    }
  }
}

void SHA256_Final(unsigned char hash[32], SHA256_CTX *ctx) {
  ctx->bitlen += static_cast<uint64_t>(ctx->datalen) * 8ULL;

  ctx->data[ctx->datalen++] = 0x80U;
  if (ctx->datalen > 56) {
    while (ctx->datalen < 64) ctx->data[ctx->datalen++] = 0;
    transform(ctx, ctx->data);
    ctx->datalen = 0;
  }
  while (ctx->datalen < 56) ctx->data[ctx->datalen++] = 0;

  for (int i = 7; i >= 0; --i) {
    ctx->data[ctx->datalen++] = static_cast<unsigned char>((ctx->bitlen >> (i * 8)) & 0xffU);
  }

  transform(ctx, ctx->data);

  for (unsigned i = 0; i < 8; ++i) {
    hash[i * 4 + 0] = static_cast<unsigned char>((ctx->state[i] >> 24) & 0xffU);
    hash[i * 4 + 1] = static_cast<unsigned char>((ctx->state[i] >> 16) & 0xffU);
    hash[i * 4 + 2] = static_cast<unsigned char>((ctx->state[i] >> 8) & 0xffU);
    hash[i * 4 + 3] = static_cast<unsigned char>((ctx->state[i]) & 0xffU);
  }
}


std::string Sha256Hex(std::string_view input) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, reinterpret_cast<const unsigned char *>(input.data()), input.size());
  unsigned char digest[32];
  SHA256_Final(digest, &ctx);

  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    result[i * 2] = kHex[(digest[i] >> 4) & 0x0f];
    result[i * 2 + 1] = kHex[digest[i] & 0x0f];
  }
  return result;
}