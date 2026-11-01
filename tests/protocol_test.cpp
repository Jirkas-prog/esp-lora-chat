#include "protocol.h"
#include "ota_package.h"
#include "lab_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <vector>
using namespace chat;
int main() {
  // Every legal file length must round-trip exactly; exercise final short blocks.
  for (uint32_t size = 1; size <= MAX_FILE; ++size) {
    uint32_t total = 0;
    for (uint16_t i = 0; i < chunks(size); ++i) {
      size_t n = chunkSize(size, i);
      assert(n > 0 && n <= CHUNK);
      total += n;
    }
    assert(total == size);
    assert(chunkSize(size, chunks(size)) == 0);
    assert(chunkSize(size, END_INDEX) == 0);
  }
  assert(sizeof(Header) == 25 && chunks(MAX_FILE) == 365);
  Header h{0xC74A, 1, DATA, 1, 2, 123, 1, 456, 0, 180};
  const size_t frameSize = sizeof(Header) + CHUNK + TAG_SIZE;
  assert(validHeader(h, frameSize, 2));
  for (size_t n = 0; n < 256; ++n) if (n != frameSize) assert(!validHeader(h, n, 2));
  assert(!validHeader(h, frameSize, 1));
  Header bad = h; bad.length = 181; assert(!validHeader(bad, frameSize + 1, 2));
  bad = h; bad.counter = 0; assert(!validHeader(bad, frameSize, 2));
  bad = h; bad.type = 99; assert(!validHeader(bad, frameSize, 2));
  bad = h; bad.version = 2; assert(!validHeader(bad, frameSize, 2));
  bad = h; bad.magic = 0; assert(!validHeader(bad, frameSize, 2));
  bad = h; bad.type = BENCH_DATA; bad.length = MAX_FRAME_PAYLOAD;
  assert(validHeader(bad, 255, 2));
  bad.length++; assert(!validHeader(bad, 256, 2));
  bad = h; bad.type=PROFILE_PROOF; bad.length=3;
  assert(validHeader(bad,sizeof(Header)+3+TAG_SIZE,2));
  for (uint8_t p = 0; p < PROFILE_COUNT; ++p) {
    const auto &profile=PROFILES[p];
    if(loraProfile(p)) {
      assert(profile.sf>=5 && profile.sf<=12 && profile.rxBandwidth==125);
      assert(profile.preamble >= (profile.sf<=6 ? 12 : 8));
    } else {
      // Approximate Carson bandwidth and configured RX filter both stay inside
      // this 250 kHz channel. This is not a substitute for an RF spectrum test.
      assert(profile.kbps>0 && profile.kbps+2*profile.deviation<=250);
      assert(profile.rxBandwidth>=profile.kbps+2*profile.deviation && profile.rxBandwidth<=250);
    }
    assert(validLabRequest({CHANGE_PROFILE, p, 0, 0}));
    for (uint8_t d = 1; d <= 3; ++d) {
      assert(validLabRequest({START_BENCH, p, d, 15}));
      assert(validLabRequest({START_BENCH, p, d, 600}));
    }
  }
  assert(!validLabRequest({CHANGE_PROFILE, PROFILE_COUNT, 0, 0}));
  assert(!validLabRequest({CHANGE_PROFILE, 0, 1, 0}));
  assert(!validLabRequest({START_BENCH, 0, 0, 30}));
  assert(!validLabRequest({START_BENCH, 0, 4, 30}));
  assert(!validLabRequest({START_BENCH, 0, 3, 14}));
  assert(!validLabRequest({START_BENCH, 0, 3, 601}));
  BenchRx rx;
  assert(rx.rate(0) == 0 && rx.rate(5000) == 0);
  assert(rx.add(0, 1000) && rx.add(2, 2000));
  assert(!rx.add(1, 2100) && !rx.add(2, 2200));
  assert(rx.received == 2 && rx.expected == 3);
  assert(rx.rate(2000) == 189 && rx.rate(6000) == 37 && rx.rate(7000) == 0);
  for (uint16_t i = 3; i < 100; ++i) assert(rx.add(i, i * 250));
  assert(rx.rate(25000) == 19 * BENCH_BYTES / 5); // Ring wrapped, only last 5 s.
  assert(gfskAirtimeUs(255, 100) == 21280 && gfskAirtimeUs(255, 50) == 42560);
  for (uint32_t air : {21280U, 42560U, 399616U, 1250304U}) {
    uint32_t cycle = benchCycle(air);
    assert(cycle * 1000 >= air * 10);
    for (uint32_t t = 0; t < cycle * 2; ++t)
      assert(!(benchSlot(t, cycle, 1) && benchSlot(t, cycle, 2)));
    assert(benchSlot(0, cycle, 1) && benchSlot(cycle / 2, cycle, 2));
  }
  assert(due(10, 10) && due(11, 10) && !due(9, 10));
  assert(due(3, 0xfffffff0) && !due(0xfffffff0, 3));
  // All frame sizes including small ACKs get at least 9x airtime off.
  for (uint32_t us = 1; us < 1000000; us += 137) assert(uint64_t(offTime(us)) * 1000 >= uint64_t(us) * 9);
  OtaHeader ota{};
  memcpy(ota.magic, "LCHATOTA", 8);
  ota.version = 1; ota.node = 1; ota.protocol = 1; ota.size = 900000;
  assert(validOtaHeader(ota, 1) && !validOtaHeader(ota, 2));
  ota.size = MAX_FIRMWARE + 1; assert(!validOtaHeader(ota, 1));
  ota.size = 0; assert(!validOtaHeader(ota, 1));
  ota.size = MAX_FIRMWARE; assert(validOtaHeader(ota, 1));
  ota.protocol = 2; assert(!validOtaHeader(ota, 1));
  uint8_t a[32] = {}, b[32] = {};
  assert(equalDigest(a, b));
  for (size_t i = 0; i < 32; ++i) { b[i] = 1; assert(!equalDigest(a, b)); b[i] = 0; }
  puts("PASS: transfer sizes, frames, benchmark loss/rates/slots, GFSK airtime, timers, duty cycle, OTA");
}
