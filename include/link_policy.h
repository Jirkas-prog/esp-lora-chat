#pragma once
#include "protocol.h"

namespace chat {
constexpr uint16_t HELLO_REQUEST_REPLY = 1, REPORT_FINAL_SEEN = 1;
constexpr uint32_t LINK_STALE_MS = 90000, LINK_OFFLINE_MS = 120000;
inline uint32_t probeDelay(uint8_t attempt, uint32_t random) {
  uint32_t delay = 400U << (attempt < 5 ? attempt : 5);
  return delay + random % (delay / 3 + 1); // 0.4 s -> about 13..17 s, no synchronized retry storm.
}
inline uint32_t reportInterval(uint32_t airtimeUs) {
  // Steady-state reports: at most one / 10 s, <=2.5% TX time even on slow LoRa.
  uint32_t budget = ((airtimeUs + 999) / 1000) * 40;
  return budget > 10000 ? budget : 10000;
}
inline bool feedbackFits(uint32_t now, uint32_t until, uint32_t airtimeUs) {
  return int32_t(until - now) > int32_t((airtimeUs + 999) / 1000 + 25);
}
struct FinalReports {
  uint8_t sent = 0;
  bool peerHasOurs = false, receivedPeer = false, toldPeer = false;
  void received(uint16_t flags) {
    receivedPeer = true;
    if (flags & REPORT_FINAL_SEEN) peerHasOurs = true;
  }
  bool needed() const { return sent < 3 && (!peerHasOurs || (receivedPeer && !toldPeer)); }
  void transmitted() { ++sent; toldPeer = receivedPeer; }
};
struct HelloResult { bool accepted = false, changed = false; };
struct PeerLink {
  uint64_t session = 0, candidate = 0, challenge = 0, peerChallenge = 0;
  uint32_t counter = 0, candidateCounter = 0, lastRx = 0, helloAt = 0, searchSince = 0;
  uint8_t attempt = 0;
  bool searching = true, replyPending = false;
  uint8_t node = 1;

  void reset(uint32_t now, uint64_t nonce, uint32_t random, uint8_t self, uint32_t airtimeUs = 0) {
    session = candidate = peerChallenge = 0; counter = candidateCounter = 0;
    challenge = nonce; node = self; lastRx = 0; searchSince = now;
    attempt = 0; searching = true; replyPending = false;
    helloAt = now + (self == 1 ? 80 : 300 + (airtimeUs + 999) / 1000) + random % 180;
  }
  void contact(uint32_t now) {
    lastRx = now; searching = false; attempt = 0;
    if (!replyPending) helloAt = now + (node == 1 ? 60000 : 75000);
  }
  void request(uint32_t now) {
    if (!searching) { searching = true; searchSince = now; attempt = 0; helloAt = now; }
  }
  void replySoon(uint32_t now, uint32_t random) {
    if (replyPending) return; // A replay cannot postpone an already queued answer.
    replyPending = true; helloAt = now + 30 + random % 90;
  }
  HelloResult hello(uint64_t sid, uint32_t serial, uint64_t remoteChallenge, uint64_t echo,
                    uint16_t flags, uint32_t now, uint64_t nonce, uint32_t random) {
    if (!sid || !serial || !remoteChallenge || (flags & ~HELLO_REQUEST_REPLY)) return {};
    bool changed = session != sid, newCandidate = candidate != sid;
    if (!changed && serial <= counter) return {};
    if (newCandidate) {
      candidate = sid; candidateCounter = 0; challenge = nonce;
      searching = true; searchSince = now; attempt = 0;
    }
    if (changed && serial <= candidateCounter) return {};
    candidateCounter = serial;
    bool answer = newCandidate || peerChallenge != remoteChallenge || echo != challenge || (flags & HELLO_REQUEST_REPLY);
    peerChallenge = remoteChallenge;
    HelloResult result;
    if (echo == challenge) {
      session = sid; counter = serial; contact(now);
      result.accepted = true; result.changed = changed;
    }
    if (answer) replySoon(now, random);
    return result;
  }
  bool packet(uint64_t sid, uint32_t serial, uint32_t now) {
    if (!session || session != sid) { request(now); return false; }
    if (!serial || serial <= counter) return false;
    counter = serial; contact(now); return true;
  }
  bool urgent(uint32_t now) const { return (replyPending || searching || !session) && due(now, helloAt); }
  bool idleDue(uint32_t now) const { return !searching && !replyPending && session && due(now, helloAt); }
  bool wantReply() const { return searching || session != candidate || !session; }
  void transmitted(uint32_t now, bool success, uint32_t random, uint32_t airtimeUs = 0) {
    if (success) replyPending = false;
    if (!success || wantReply()) {
      uint32_t delay = probeDelay(attempt, random);
      // A fixed duty pause must not erase the jitter and synchronize both radios.
      uint32_t radioDelay = offTime(airtimeUs) + random % (2 * ((airtimeUs + 999) / 1000) + 1);
      helloAt = now + (delay > radioDelay ? delay : radioDelay);
      if (attempt < 5) ++attempt;
    } else helloAt = now + (node == 1 ? 60000 : 75000);
  }
};
}
