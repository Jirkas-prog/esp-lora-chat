#pragma once
#include "protocol.h"

namespace chat {
constexpr uint32_t LAB_CAPABILITY = 0x4c414200;
constexpr uint32_t SIM_CAPABILITY = LAB_CAPABILITY | 0x80;
constexpr uint32_t SWITCH_CAPABILITY = SIM_CAPABILITY | 0x40;
constexpr uint8_t CHANGE_PROFILE = 1, START_BENCH = 2, START_SIM = 3;
struct RadioProfile {
  const char *name, *hint;
  uint8_t sf, preamble; // sf=0 selects GFSK; all LoRa profiles use BW125 and CR4/5.
  uint16_t kbps;
  float deviation, rxBandwidth;
};
// Keep IDs 0..3 stable for existing settings, exports and older firmware.
const RadioProfile PROFILES[] = {
  {"LoRa SF7 / 125 kHz", "Výchozí kompromis dosahu a rychlosti.", 7, 8, 0, 0, 125},
  {"LoRa SF9 / 125 kHz", "Vyšší citlivost, nejnižší propustnost.", 9, 8, 0, 0, 125},
  {"GFSK 50 kbit/s", "Rychlý přenos, porovnej s dosahem LoRa.", 0, 32, 50, 25, 117.3},
  {"GFSK 100 kbit/s", "Rychlý GMSK profil; skutečné B/s změří test.", 0, 32, 100, 25, 234.3},
  {"LoRa SF6 / 125 kHz", "Rychlejší LoRa, menší rezerva slabého signálu.", 6, 12, 0, 0, 125},
  {"LoRa SF5 / 125 kHz", "Nejrychlejší zdejší LoRa; otestuj dosah.", 5, 12, 0, 0, 125},
  {"GFSK 150 kbit/s", "Nejrychlejší GMSK profil; nejprve přepni zblízka.", 0, 32, 150, 37.5, 234.3},
  {"GFSK 25 kbit/s", "Užší přijímací pásmo; kandidát na větší dosah než rychlé GFSK.", 0, 32, 25, 12.5, 58.6}
};
constexpr uint8_t PROFILE_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);
inline bool loraProfile(uint8_t profile) { return profile < PROFILE_COUNT && PROFILES[profile].sf != 0; }
#pragma pack(push, 1)
struct LabRequest {
  uint8_t action, profile, direction; // direction bit 0: A->B, bit 1: B->A
  uint16_t seconds;
};
struct BenchReport {
  uint32_t elapsed, sent, received, expected, rate, txErrors;
  uint8_t active;
};
#pragma pack(pop)
constexpr size_t BENCH_BYTES = MAX_FRAME_PAYLOAD - sizeof(BenchReport);
static_assert(BENCH_BYTES == 189, "Update benchmark byte accounting if report changes");
inline bool validLabRequest(const LabRequest &r) {
  if (r.profile >= PROFILE_COUNT) return false;
  if (r.action == CHANGE_PROFILE) return r.direction == 0 && r.seconds == 0;
  if (r.action == START_SIM) return (r.direction == 1 || r.direction == 2) && r.seconds >= 30 && r.seconds <= 3600;
  return r.action == START_BENCH && r.direction >= 1 && r.direction <= 3 &&
    r.seconds >= 15 && r.seconds <= 600;
}
inline uint8_t testByte(uint32_t id, uint16_t sequence, size_t offset) {
  return uint8_t(id + sequence * 17U + offset * 31U);
}
struct BenchRx {
  uint32_t received = 0, expected = 0;
  uint32_t times[64] = {};
  bool add(uint16_t sequence, uint32_t elapsed) {
    if (sequence < expected) return false;
    times[received % 64] = elapsed;
    ++received; expected = uint32_t(sequence) + 1;
    return true;
  }
  uint32_t rate(uint32_t elapsed) const {
    if (!elapsed) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < received && i < 64; ++i)
      if (elapsed - times[i] < 5000) ++count;
    return count * BENCH_BYTES * 1000 / (elapsed < 5000 ? elapsed : 5000);
  }
};
inline uint32_t benchCycle(uint32_t airtimeUs) {
  // One data frame per station per cycle; leave room for timing jitter.
  return ((airtimeUs + 999) / 1000) * 10 + 40;
}
inline uint32_t gfskAirtimeUs(size_t frameBytes, uint32_t kbps) {
  // Our packet: 32-bit preamble + 32-bit sync + length byte + 16-bit CRC.
  return uint32_t((frameBytes + 11) * 8 * 1000 + kbps - 1) / kbps;
}
inline bool benchSlot(uint32_t elapsed, uint32_t cycle, uint8_t node) {
  uint32_t phase = elapsed % cycle, offset = node == 1 ? 0 : cycle / 2;
  return phase >= offset && phase < offset + cycle / 4;
}
}
