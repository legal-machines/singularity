#include "Platform/Sha256/Sha256.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace singularity::platform {
namespace {

#if defined(__APPLE__)

// The system's own SHA-256, which runs on the processor's SHA-2 instructions.
// Papinian's hash-pinned 631 MB package is verified on every launch (and
// again around its load); through the portable code below that took about
// 10 s per pass in a Debug build and 1.5 s in Release, here about 0.3 s. The
// digest is the same FIPS 180-4 value either way.
class Sha256State final {
public:
  Sha256State() { CC_SHA256_Init(&context_); }

  void update(const unsigned char *data, std::size_t length) {
    // CC_LONG is 32-bit: a longer input is fed in bounded pieces.
    while (length > 0) {
      const std::size_t piece = std::min<std::size_t>(length, std::size_t{1} << 30);
      CC_SHA256_Update(&context_, data, static_cast<CC_LONG>(piece));
      data += piece;
      length -= piece;
    }
  }

  std::string hex_digest() {
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
    CC_SHA256_Final(digest.data(), &context_);

    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (const unsigned char byte : digest) {
      text << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return text.str();
  }

private:
  CC_SHA256_CTX context_;
};

#else

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t rotr(std::uint32_t value, unsigned int bits) {
  return (value >> bits) | (value << (32 - bits));
}

// Minimal streaming SHA-256 (FIPS 180-4). Kept private to this translation
// unit; callers only need the file-digest entry point below.
class Sha256State final {
public:
  void absorb(const unsigned char *data, std::size_t length) {
    if (buffer_length_ > 0) {
      const auto needed = std::size_t{64} - buffer_length_;
      const auto take = std::min(needed, length);
      std::memcpy(buffer_.data() + buffer_length_, data, take);
      buffer_length_ += take;
      data += take;
      length -= take;
      if (buffer_length_ == 64) {
        process_block(buffer_.data());
        buffer_length_ = 0;
      }
    }

    while (length >= 64) {
      process_block(data);
      data += 64;
      length -= 64;
    }

    if (length > 0) {
      std::memcpy(buffer_.data(), data, length);
      buffer_length_ = length;
    }
  }

  void update(const unsigned char *data, std::size_t length) {
    total_length_ += length;
    absorb(data, length);
  }

  std::string hex_digest() {
    const std::uint64_t bit_length = total_length_ * 8;

    const auto pad_length = buffer_length_ < 56 ? 56 - buffer_length_
                                                  : 120 - buffer_length_;
    std::array<unsigned char, 64> padding{};
    padding[0] = 0x80;
    absorb(padding.data(), pad_length);

    std::array<unsigned char, 8> length_bytes{};
    for (int i = 0; i < 8; ++i) {
      length_bytes[7 - i] =
          static_cast<unsigned char>((bit_length >> (i * 8)) & 0xff);
    }
    absorb(length_bytes.data(), length_bytes.size());

    std::ostringstream digest;
    digest << std::hex << std::setfill('0');
    for (const auto word : state_) {
      digest << std::setw(8) << word;
    }
    return digest.str();
  }

private:
  void process_block(const unsigned char *block) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
             (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
             static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
      const auto s0 =
          rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 =
          rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    auto e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
      const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const auto ch = (e & f) ^ (~e & g);
      const auto temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
      const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                      0xa54ff53a, 0x510e527f, 0x9b05688c,
                                      0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> buffer_{};
  std::size_t buffer_length_ = 0;
  std::uint64_t total_length_ = 0;
};

#endif

} // namespace

std::string sha256_hex_digest(const std::string &value) {
  Sha256State hasher;
  hasher.update(reinterpret_cast<const unsigned char *>(value.data()), value.size());
  return hasher.hex_digest();
}

std::string sha256_hex_digest_of_file(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Unable to open file for hashing: " +
                             path.string());
  }

  Sha256State hasher;
  std::array<unsigned char, 1 << 16> buffer{};
  while (file) {
    file.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
    const auto read_count = file.gcount();
    if (read_count > 0) {
      hasher.update(buffer.data(), static_cast<std::size_t>(read_count));
    }
  }
  if (file.bad()) {
    throw std::runtime_error("Failed reading file for hashing: " +
                             path.string());
  }

  return hasher.hex_digest();
}

} // namespace singularity::platform
