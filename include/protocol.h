#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace chat {
constexpr size_t CHUNK = 180;
constexpr size_t MAX_FRAME_PAYLOAD = 214;
constexpr uint32_t MAX_FILE = 65536;
constexpr uint32_t MAX_TEXT = 2048;
constexpr uint16_t BEGIN_INDEX = 65534, END_INDEX = 65535;
enum Type : uint8_t { HELLO = 1, BEGIN = 2, DATA = 3, END = 4, ACK = 5,
  LAB_REQUEST = 6, LAB_REPLY = 7, LAB_COMMIT = 8, BENCH_DATA = 9, BENCH_REPORT = 10, LAB_STOP = 11,
  SIM_DATA = 12, SIM_REPORT = 13, PROFILE_PROOF = 14 };
enum Result : uint8_t { OK = 0, BUSY = 1, INVALID = 2, STORAGE = 3, HASH = 4 };
// All nodes are little-endian ESP32. Header is authenticated as AES-GCM AAD.
#pragma pack(push, 1)
struct Header {
  uint16_t magic;
  uint8_t version, type, from, to;
  uint64_t session;
  uint32_t counter, transfer;
  uint16_t index;
  uint8_t length;
};
#pragma pack(pop)
constexpr size_t TAG_SIZE = 16;
static_assert(sizeof(Header) + CHUNK + TAG_SIZE <= 255, "SX1262 packet too large");
inline bool validHeader(const Header &h, size_t size, uint8_t self) {
  return h.magic == 0xC74A && h.version == 1 && h.from == 3 - self &&
    h.to == self && h.type >= HELLO && h.type <= PROFILE_PROOF && h.counter != 0 &&
    h.length <= (h.type == BENCH_DATA || h.type == SIM_DATA ? MAX_FRAME_PAYLOAD : CHUNK) &&
    size == sizeof(Header) + h.length + TAG_SIZE;
}
inline uint16_t chunks(uint32_t size) { return (size + CHUNK - 1) / CHUNK; }
inline size_t chunkSize(uint32_t size, uint16_t index) {
  uint32_t offset = uint32_t(index) * CHUNK;
  return offset >= size ? 0 : (size - offset < CHUNK ? size - offset : CHUNK);
}
inline bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
inline uint32_t offTime(uint32_t airtimeUs) {
  // <= 10% duty cycle, including ACKs, discovery and retransmissions.
  return ((airtimeUs + 999) / 1000) * 9 + 20;
}
}
