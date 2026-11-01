#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace chat {
constexpr uint32_t MAX_FIRMWARE = 0x300000;
#pragma pack(push, 1)
struct OtaHeader {
  char magic[8];
  uint8_t version, node, protocol, reserved;
  uint32_t size;
  uint8_t sha256[32];
  uint8_t hmac[32];
};
#pragma pack(pop)
static_assert(sizeof(OtaHeader) == 80, "OTA package layout changed");
inline bool validOtaHeader(const OtaHeader &h, uint8_t node) {
  return memcmp(h.magic, "LCHATOTA", 8) == 0 && h.version == 1 &&
    h.node == node && h.protocol == 1 && h.reserved == 0 &&
    h.size >= 256 && h.size <= MAX_FIRMWARE;
}
inline bool equalDigest(const uint8_t *a, const uint8_t *b) {
  uint8_t different = 0;
  for (size_t i = 0; i < 32; ++i) different |= a[i] ^ b[i];
  return different == 0;
}
}
