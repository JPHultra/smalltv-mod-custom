// Small RFC 1321 MD5 implementation for HTTP Digest authentication.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace emu {

inline uint32_t md5_rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

inline void md5_transform(uint32_t state[4], const uint8_t block[64]) {
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
      0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
      0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
      0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
      0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
      0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
      0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
      0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
      0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7,
                            12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9,
                            14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23,
                            4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10,
                            15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t M[16];
  for (int i = 0; i < 16; i++)
    M[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
           ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  for (int i = 0; i < 64; i++) {
    uint32_t f, g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
    else { f = c ^ (b | ~d); g = (7 * i) & 15; }
    uint32_t temp = d;
    d = c;
    c = b;
    b = b + md5_rotl(a + f + K[i] + M[g], R[i]);
    a = temp;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

inline std::string md5_hex(const std::string& input) {
  uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  std::string data = input;
  const uint64_t bit_len = (uint64_t)data.size() * 8;
  data.push_back((char)0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int i = 0; i < 8; i++) data.push_back((char)((bit_len >> (8 * i)) & 0xff));
  for (size_t i = 0; i < data.size(); i += 64)
    md5_transform(state, (const uint8_t*)data.data() + i);
  char out[33];
  for (int i = 0; i < 4; i++)
    snprintf(out + i * 8, 9, "%08x", state[i]);
  // The above prints little-endian words; swap each word's bytes for RFC output.
  for (int i = 0; i < 4; i++) {
    char tmp[9];
    snprintf(tmp, sizeof(tmp), "%02x%02x%02x%02x",
             (state[i] >> 0) & 0xff, (state[i] >> 8) & 0xff,
             (state[i] >> 16) & 0xff, (state[i] >> 24) & 0xff);
    memcpy(out + i * 8, tmp, 8);
  }
  out[32] = 0;
  return std::string(out);
}

}  // namespace emu
