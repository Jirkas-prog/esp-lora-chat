// Executes the production simulation state machines with a deterministic clock
// and lossy radio adapter. This is a host test, not a physical range measurement.
#include "rtk_sim.h"
#include <algorithm>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
using namespace chat;
using std::min; using std::max;
struct JsonObject {
  JsonObject operator[](const char *) { return {}; }
  template<class T> JsonObject &operator=(const T &) { return *this; }
  template<class T> T to() { return {}; }
};
using JsonDocument = JsonObject;
uint32_t clockMs = 0;
uint32_t millis() { return clockMs; }
struct Packet { uint8_t sender; Header h; std::vector<uint8_t> data; };
std::vector<Packet> wire;
bool dropMiddle = false, blackHole = false, corrupt = false, failTx = false;
uint32_t txAvailable[3] = {}, liveReports[3] = {}, finalReports[3] = {};
uint32_t airTime(size_t size, uint8_t profile) {
  const auto &p = PROFILES[profile];
  if (!p.sf) return gfskAirtimeUs(size, p.kbps);
  uint32_t sf=p.sf, bits=8*size+16-4*sf+(sf<7?0:8)+20;
  uint32_t symbols4=(p.preamble+8)*4+(sf<7?25:17)+((bits+4*sf-1)/(4*sf))*5*4;
  return symbols4*(1U<<sf)*2; // SX126x formula, BW125 / CR4/5 / explicit / CRC.

}
bool transmit(uint8_t sender, uint8_t profile, uint32_t &nextTx, bool &ok, uint8_t type, uint32_t id, uint16_t index, const uint8_t *data, size_t n) {
  assert(due(clockMs, nextTx));
  uint32_t air = airTime(sizeof(Header) + n + TAG_SIZE, profile);
  if (type == SIM_REPORT) {
    SimReport r; memcpy(&r, data, sizeof(r));
    if (r.active) {
      ++liveReports[sender];
      assert(feedbackFits(clockMs, txAvailable[3-sender] - 40, air));
    } else ++finalReports[sender];
  }
  clockMs += (air + 999) / 1000; nextTx = clockMs + offTime(air);
  txAvailable[sender] = nextTx;
  ok = !failTx;
  if (!ok || blackHole || (dropMiddle && type == SIM_DATA && index == 1)) return true;
  Packet p{}; p.sender = sender; p.h.type = type; p.h.transfer = id; p.h.index = index; p.h.length = uint8_t(n);
  if (n) p.data.assign(data, data + n);
  if (corrupt && type == SIM_DATA) p.data.back() ^= 1;
  wire.push_back(p); return true;
}
#define NODE_ID 1
namespace a {
uint8_t activeProfile = 3;
bool radioReady = true, lastTransmitOK = false;
uint32_t nextTx = 0;
std::string labMessage;
uint32_t packetAirtime(size_t n) { return airTime(n, activeProfile); }
bool transmitPacket(uint8_t type, uint32_t id, uint16_t index, const uint8_t *data, size_t n) { return transmit(NODE_ID, activeProfile, nextTx, lastTransmitOK, type, id, index, data, n); }
#include "../src/rtk_sim_runtime.h"
}
#undef NODE_ID
#define NODE_ID 2
namespace b {
uint8_t activeProfile = 3;
bool radioReady = true, lastTransmitOK = false;
uint32_t nextTx = 0;
std::string labMessage;
uint32_t packetAirtime(size_t n) { return airTime(n, activeProfile); }
bool transmitPacket(uint8_t type, uint32_t id, uint16_t index, const uint8_t *data, size_t n) { return transmit(NODE_ID, activeProfile, nextTx, lastTransmitOK, type, id, index, data, n); }
#include "../src/rtk_sim_runtime.h"
}
#undef NODE_ID
void deliver() {
  auto packets = wire; wire.clear();
  for (auto &p : packets) {
    if (p.h.type == LAB_STOP) {
      if (p.sender == 1) { b::stopSimulation(false); b::sim.reportAt = 0; }
      else { a::stopSimulation(false); a::sim.reportAt = 0; }
    } else if (p.sender == 1) b::receiveSimulation(p.h, p.data.data());
    else a::receiveSimulation(p.h, p.data.data());
  }
}
void run(uint32_t duration) {
  uint32_t until = clockMs + duration;
  while (!due(clockMs, until)) {
    a::maintainSimulation(); b::maintainSimulation();
    if (due(clockMs, a::nextTx)) a::tickSimulation(); deliver();
    if (due(clockMs, b::nextTx)) b::tickSimulation(); deliver();
    ++clockMs;
  }
}
void start(uint8_t profile, SimOptions options, uint8_t direction = 1, uint16_t seconds = 60, uint32_t time = 1000) {
  clockMs = time; wire.clear(); dropMiddle = blackHole = corrupt = failTx = false;
  for (auto i : {1,2}) { txAvailable[i] = time; liveReports[i] = finalReports[i] = 0; }
  a::activeProfile = b::activeProfile = profile; a::nextTx = b::nextTx = clockMs;
  LabRequest r{START_SIM, profile, direction, seconds};
  assert(validLabRequest(r) && validSimOptions(options));
  a::startSimulation(r, options, 42); b::startSimulation(r, options, 42);
}
int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  assert(crc24q(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xcde703);
  for (uint8_t preset = 0; preset < 3; ++preset) {
    uint8_t data[SIM_MAX]; size_t size = makeSimEpoch(data, preset, 42, 7);
    assert(size == simSize(preset) && validRtcmFrames(data, size));
    for (size_t i = 0; i < size; ++i) { data[i] ^= 1; assert(!validRtcmFrames(data, size)); data[i] ^= 1; }
    SimAssembly rx; rx.active = true; rx.size = uint16_t(size);
    for (size_t i = 0; i < (size + SIM_CHUNK - 1) / SIM_CHUNK; ++i) {
      size_t n = min(SIM_CHUNK, size - i * SIM_CHUNK);
      assert(rx.add(uint16_t(i), data + i * SIM_CHUNK, n));
      assert(!rx.add(uint16_t(i), data + i * SIM_CHUNK, n));
    }
    assert(rx.complete() && !memcmp(rx.data, data, size));
  }
  assert(!validSimOptions({3, 0, 20000}) && !validSimOptions({1, 3, 20000}) && !validSimOptions({1, 0, 0}));
  assert(!validLabRequest({START_SIM, 0, 3, 60}) && !validLabRequest({START_SIM, 0, 1, 3601}));
  for (uint8_t profile = 0; profile < PROFILE_COUNT; ++profile) for (uint8_t preset = 0; preset < 3; ++preset) {
    start(profile, {preset, 0, 20000}); run(81000);
    assert(!a::sim.active && !b::sim.active);
    assert(a::sim.counts.sent == b::sim.counts.complete);
    assert(b::sim.counts.completeBytes == b::sim.counts.complete * simSize(preset));
    assert(b::sim.counts.bytes >= b::sim.counts.completeBytes && b::sim.counts.bad == 0);
    assert(a::sim.counts.busyDrops == 0);
    assert(a::sim.counts.expired <= 4); // No busy-loop generation in the last unsendable milliseconds.
    assert(liveReports[1] == 0 && liveReports[2] <= 6);
    assert(finalReports[1] <= 3 && finalReports[2] <= 3);
    printf("MODEL profile=%u preset=%u: sent=%u received=%u expired=%u\n", profile, preset, a::sim.counts.sent, b::sim.counts.complete, a::sim.counts.expired);
  }
  start(3, {2, 1000, 20000}); run(81000);
  assert(a::sim.counts.generated == 60 && a::sim.counts.busyDrops > 0);
  assert(a::sim.counts.generated == a::sim.counts.busyDrops + a::sim.counts.sent + a::sim.counts.expired);
  start(3, {1, 0, 20000}); dropMiddle = true; run(81000);
  assert(!b::sim.counts.complete && b::sim.counts.partial > 0 && b::sim.counts.bytes > 0);
  assert(b::simReport().lastAge == SIM_UNKNOWN && b::simReport().usefulRate == 0);
  start(3, {1, 0, 20000}); corrupt = true; run(81000);
  assert(!b::sim.counts.complete && b::sim.counts.bad > 0);
  start(1, {2, 0, 2000}); run(81000);
  assert(a::sim.counts.expired > 0 && !b::sim.counts.complete);
  start(3, {1, 0, 20000}, 1, 300); run(10000);
  auto received = b::sim.counts.complete; blackHole = true; run(1000);
  auto reports = liveReports[2]; run(69000); assert(liveReports[2] == reports);
  assert(a::sim.active && b::sim.active && b::sim.counts.complete == received);
  assert(b::simReport().lastAge >= 70000 && b::simReport().rate == 0);
  blackHole = false; run(10000); assert(b::sim.counts.complete > received);
  b::stopSimulation(true); run(10000); assert(!a::sim.active && !b::sim.active && !b::sim.stopping);
  start(3, {1, 0, 20000}, 2, 60, UINT32_MAX - 10000); run(81000);
  assert(b::sim.counts.sent > 0 && b::sim.counts.sent == a::sim.counts.complete);
  start(3, {1, 1000, 20000}); failTx = true; run(81000);
  assert(!a::sim.counts.sent && a::sim.counts.txErrors > 0 && !b::sim.counts.complete);
  puts("PASS: production RTK simulator, RTCM framing/CRC, all profiles/presets, no queue, drops, loss, expiry, outage recovery, stop, timer rollover");
}
