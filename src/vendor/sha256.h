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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct SHA256_CTX {  // NOLINT
  uint32_t state[8];
  uint64_t bitlen;
  uint32_t datalen;
  unsigned char data[64];
};

// NOLINTNEXTLINE
void SHA256_Init(SHA256_CTX *ctx);
// NOLINTNEXTLINE
void SHA256_Update(SHA256_CTX *ctx, const unsigned char *data, std::size_t len);
// NOLINTNEXTLINE
void SHA256_Final(unsigned char hash[32], SHA256_CTX *ctx);

std::string Sha256Hex(std::string_view input);

bool IsValidSha256Hex(std::string_view value);
