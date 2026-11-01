#pragma once
#include "lab_protocol.h"
#include "link_policy.h"
#include <new>

namespace chat {
#pragma pack(push, 1)
struct SimOptions { uint8_t preset; uint16_t intervalMs, ttlMs; };
struct SimRequest { LabRequest lab; SimOptions options; };
struct SimFragment {
  uint32_t epoch, generated, busyDrops, expired;
  uint16_t ageMs, size;
};
// Snapshot, never an acknowledgement or a request to retransmit.
struct SimReport {
  uint32_t elapsed, generated, busyDrops, expired, sent, txErrors;
  uint32_t complete, partial, bad, fragments, bytes, completeBytes;
  uint32_t rate, usefulRate, lastAge, lastInterval, maxGap, meanInterval, deliveryAge;
  uint32_t firstEpoch, lastEpoch;
  uint8_t active;
};
#pragma pack(pop)
constexpr size_t SIM_CHUNK = MAX_FRAME_PAYLOAD - sizeof(SimFragment);
constexpr size_t SIM_MAX = 1141;
constexpr uint32_t SIM_UNKNOWN = UINT32_MAX;
inline bool validSimOptions(const SimOptions &o) {
  return o.preset < 3 && (o.intervalMs == 0 || o.intervalMs == 1000 || o.intervalMs == 2000 ||
    o.intervalMs == 5000 || o.intervalMs == 10000) &&
    (o.ttlMs == 2000 || o.ttlMs == 5000 || o.ttlMs == 10000 || o.ttlMs == 20000);
}
inline size_t simSize(uint8_t preset) { return preset == 0 ? 387 : preset == 1 ? 733 : 1141; }
inline uint32_t crc24q(const uint8_t *p, size_t n) {
  uint32_t crc = 0;
  while (n--) {
    crc ^= uint32_t(*p++) << 16;
    for (int b = 0; b < 8; ++b) { crc <<= 1; if (crc & 0x1000000) crc ^= 0x1864cfb; }
  }
  return crc & 0xffffff;
}
inline size_t syntheticRtcm(uint8_t *out, uint16_t type, size_t payload, uint32_t id, uint32_t epoch) {
  out[0] = 0xd3; out[1] = (payload >> 8) & 3; out[2] = payload & 255;
  for (size_t i = 0; i < payload; ++i) out[3 + i] = uint8_t(id + epoch * 17 + i * 31 + type);
  out[3] = type >> 4; out[4] = (type & 15) << 4; out[5] = 1; // message number + synthetic station 1
  uint32_t crc = crc24q(out, payload + 3);
  out[payload + 3] = crc >> 16; out[payload + 4] = crc >> 8; out[payload + 5] = crc;
  return payload + 6;
}
inline size_t makeSimEpoch(uint8_t *out, uint8_t preset, uint32_t id, uint32_t epoch) {
  // Traffic model: 10 satellites/constellation, two signals each. MSM4:
  // ceil((169 + 20 cells + 18*10 + 48*20)/8) = 167 payload bytes.
  // MSM7: ceil((169 + 20 + 36*10 + 80*20)/8) = 269 bytes.
  // Framing and CRC are RTCM3, observation fields are deterministic TEST DATA,
  // NOT GNSS measurements. Nothing is forwarded to a GNSS UART.
  size_t n = 0;
  const uint16_t types[] = {1074, 1084, 1094, 1124};
  for (uint8_t i = 0; i < (preset == 0 ? 2 : 4); ++i)
    n += syntheticRtcm(out + n, types[i] + (preset == 2 ? 3 : 0), preset == 2 ? 269 : 167, id, epoch);
  n += syntheticRtcm(out + n, 1005, 19, id, epoch);
  n += syntheticRtcm(out + n, 1230, 10, id, epoch);
  return n; // Metadata in every epoch is deliberately conservative.
}
inline bool validRtcmFrames(const uint8_t *p, size_t size) {
  while (size) {
    if (size < 6 || p[0] != 0xd3 || (p[1] & 0xfc)) return false;
    size_t n = (size_t(p[1]) << 8) + p[2] + 6;
    if (n > size || crc24q(p, n) != 0) return false;
    p += n; size -= n;
  }
  return true;
}
struct SimRate {
  uint32_t count = 0, times[128] = {};
  uint16_t sizes[128] = {};
  void add(uint32_t elapsed, uint16_t bytes) {
    times[count % 128] = elapsed; sizes[count++ % 128] = bytes;
  }
  uint32_t rate(uint32_t elapsed) const {
    if (!elapsed) return 0;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < count && i < 128; ++i)
      if (elapsed - times[i] < 5000) sum += sizes[i];
    return sum * 1000 / (elapsed < 5000 ? elapsed : 5000);
  }
};
struct SimAssembly {
  bool active = false;
  uint32_t epoch = 0, mask = 0, started = 0;
  uint16_t size = 0;
  uint8_t data[SIM_MAX] = {};
  // Caller expires incomplete epochs using a local deadline, and ignores old IDs.
  bool add(uint16_t index, const uint8_t *p, size_t n) {
    size_t offset = size_t(index) * SIM_CHUNK;
    if (!active || offset >= size || n != (size - offset < SIM_CHUNK ? size - offset : SIM_CHUNK) || (mask & (1U << index))) return false;
    memcpy(data + offset, p, n); mask |= 1U << index;
    return true;
  }
  bool complete() const { return active && mask == ((1U << ((size + SIM_CHUNK - 1) / SIM_CHUNK)) - 1); }
};
// A fixed-rate source drops generated epochs while busy: no pending slot/FIFO.
// Max mode generates only when the radio can start a new epoch.
inline uint32_t simDueCount(uint32_t now, uint32_t next, uint16_t interval) {
  return interval && due(now, next) ? (now - next) / interval + 1 : 0;
}
static_assert(sizeof(SimReport) <= CHUNK && SIM_CHUNK == 194, "Radio framing");
}
