#pragma once
#include "lab_protocol.h"

namespace chat {
constexpr uint32_t PROFILE_TRIAL_MS = 30000;
struct ProfileProof { uint8_t previous, target, seenPeer; };
// A trial never overwrites the saved profile before a fresh bidirectional proof.
// Finite handshakes cannot guarantee agreement under arbitrary asymmetric loss;
// the existing shared SF7 recovery remains the last rendezvous.
struct ProfileTrial {
  bool active = false, seenPeer = false, peerSawUs = false, echoed = false;
  uint8_t previous = 0, target = 0;
  uint32_t id = 0, until = 0, sendAt = 0, lastProof = 0;
  uint64_t peerBoot = 0;
  void begin(uint32_t now, uint32_t transfer, uint8_t from, uint8_t to, uint64_t boot) {
    *this = ProfileTrial();
    active = true; id = transfer; previous = from; target = to; peerBoot = boot;
    until = now + PROFILE_TRIAL_MS; sendAt = now;
  }
  bool receive(uint32_t transfer, const ProfileProof &p, uint32_t now) {
    if (!active || transfer != id || p.previous != previous || p.target != target || p.seenPeer > 1) return false;
    seenPeer = true; peerSawUs |= p.seenPeer != 0; lastProof = now;
    return true;
  }
  bool success(uint32_t now, uint64_t boot) const {
    return boot == peerBoot && seenPeer && peerSawUs && echoed && now - lastProof < 12000;
  }
};
}
