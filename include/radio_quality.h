#pragma once
#include "protocol.h"
#include <math.h>

namespace chat {
enum RxOutcome : uint8_t { RX_OK, RX_CRC, RX_HEADER, RX_FORMAT, RX_AUTH, RX_SESSION, RX_HANDSHAKE, RX_IO, RX_OUTCOMES };
struct QualityBin {
  uint32_t atMs, accepted, rejected, bytes;
  float rssiSum, snrSum, channelMin;
  uint16_t signalSamples, snrSamples, channelSamples;
  bool valid;
};
// Passive local telemetry only. No extra radio packets, no raw I/Q access.
struct RadioQuality {
  static constexpr size_t WINDOW = 120;
  QualityBin bins[WINDOW];
  uint32_t counts[RX_OUTCOMES];
  uint32_t since, generation, lastSignalAt, channelAt, sampleAt, binAt;
  uint32_t txOK, txErrors, txBytes, retries, recovered, duplicates, delivered, gaps, peerCounter;
  uint64_t peerBoot;
  float lastRssi, lastSnr, channelRssi;
  uint8_t profile, cursor;
  bool initialized, hasSignal, hasSnr, hasChannel;
  void setProfile(uint8_t next, uint32_t now) {
    if(initialized && profile==next)return;
    uint32_t gen=generation+1;
    // Avoid a multi-kilobyte temporary on the ESP32 loop stack.
    memset(this,0,sizeof(*this));
    initialized=true; profile=next; since=now; sampleAt=now; binAt=now; generation=gen;
  }
  QualityBin &bin(uint32_t now) {
    uint32_t steps=uint32_t(now-binAt)/1000;
    if(steps){ cursor=(cursor+steps)%WINDOW; binAt+=steps*1000; }
    auto &b=bins[cursor];
    if(!b.valid || b.atMs!=binAt) { memset(&b,0,sizeof(b)); b.valid=true; b.atMs=binAt; }
    return b;
  }
  bool recent(const QualityBin &b, uint32_t now) const {
    return b.valid && uint32_t(now-b.atMs)<WINDOW*1000;
  }
  void receive(uint32_t now,RxOutcome result,size_t length=0,float rssi=0,float snr=0,bool lora=false) {
    ++counts[result]; auto &b=bin(now);
    if(result!=RX_OK) {
      if(result!=RX_HANDSHAKE && result!=RX_IO)++b.rejected;
      return;
    }
    ++b.accepted; b.bytes+=length;
    if(isfinite(rssi) && rssi<0 && rssi>=-160) {
      b.rssiSum+=rssi; ++b.signalSamples; lastRssi=rssi; hasSignal=true; lastSignalAt=now;
      hasSnr=lora && isfinite(snr) && snr>=-32 && snr<=32;
      if(hasSnr){ b.snrSum+=snr; ++b.snrSamples; lastSnr=snr; }
    }
  }
  void sequence(uint64_t boot,uint32_t counter) {
    if(peerBoot==boot && peerCounter && counter>peerCounter)gaps+=counter-peerCounter-1;
    peerBoot=boot; peerCounter=counter;
  }
  void channel(uint32_t now,float rssi) {
    sampleAt=now+200;
    if(!isfinite(rssi) || rssi>=0 || rssi< -160)return;
    auto &b=bin(now);
    if(!b.channelSamples || rssi<b.channelMin)b.channelMin=rssi;
    ++b.channelSamples; channelRssi=rssi; channelAt=now; hasChannel=true;
  }
};
}
