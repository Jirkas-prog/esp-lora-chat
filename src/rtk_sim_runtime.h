// Shared authenticated transport, one in-flight epoch, no retransmissions.
struct RtkSimulation {
  bool active = false, stopping = false, transmitting = false, peerKnown = false;
  uint8_t sender = 0, profile = 0;
  SimOptions options{};
  uint32_t id = 0, started = 0, duration = 0, ended = 0, nextEpoch = 0;
  uint32_t txEpoch = 0, born = 0, peerAt = 0, reportAt = 0, reportUntil = 0;
  uint32_t feedbackUntil = 0, stopUntil = 0, stopAt = 0, lastComplete = 0, firstComplete = 0;
  uint32_t seenEpoch = 0;
  uint16_t txIndex = 0;
  uint8_t txData[SIM_MAX] = {};
  SimReport counts{}, peer{};
  SimAssembly assembly;
  SimRate traffic, useful;
  FinalReports finalReports;
} sim;
bool peerSim = false;
// Reconstruct in place: two nested 4 kB temporaries overflowed the ESP32 loop stack.
void resetSimulation() { new (&sim) RtkSimulation(); }
uint32_t simElapsed() { return sim.active ? min(uint32_t(millis() - sim.started), sim.duration) : sim.ended; }
bool simSender() { return sim.sender == NODE_ID; }
void stopSimulation(bool notifyPeer) {
  if (!sim.id) return;
  if (sim.active) {
    if (sim.transmitting) { ++sim.counts.expired; sim.transmitting = false; }
    if (sim.assembly.active) { ++sim.counts.partial; sim.assembly.active = false; }
    sim.ended = simElapsed(); sim.active = false; sim.reportUntil = millis() + 20000; sim.reportAt = 0;
    labMessage = "RTK simulace skončila. Výsledky zůstávají do dalšího testu nebo restartu.";
  }
  if (notifyPeer) { sim.stopping = true; sim.stopUntil = millis() + 20000; sim.stopAt = 0; }
}
void startSimulation(const LabRequest &r, const SimOptions &o, uint32_t id) {
  resetSimulation(); sim.active = true; sim.id = id;
  sim.sender = r.direction == 1 ? 1 : 2; sim.profile = activeProfile; sim.options = o;
  sim.started = sim.nextEpoch = millis(); sim.duration = uint32_t(r.seconds) * 1000;
  labMessage = "RTK simulace běží. Rádiový režim se při ztrátě dosahu nemění.";
}
void newSimEpoch(uint32_t epoch) {
  sim.transmitting = true; sim.txEpoch = epoch; sim.born = millis(); sim.txIndex = 0;
  makeSimEpoch(sim.txData, sim.options.preset, sim.id, epoch);
}
void maintainSimulation() {
  uint32_t now = millis();
  if (sim.active && due(now, sim.started + sim.duration)) stopSimulation(false);
  if (sim.stopping && due(now, sim.stopUntil)) {
    sim.stopping = false;
    labMessage = "Místní RTK test skončil. Pokud druhá deska Stop neslyšela, doběhne zvolenou dobu.";
  }
  if (!sim.active) return;
  if (sim.transmitting && now - sim.born >= sim.options.ttlMs) {
    sim.transmitting = false; ++sim.counts.expired;
  }
  if (sim.assembly.active && now - sim.assembly.started >= sim.options.ttlMs) {
    sim.assembly.active = false; ++sim.counts.partial;
  }
  if (simSender() && sim.options.intervalMs) {
    uint32_t count = simDueCount(now, sim.nextEpoch, sim.options.intervalMs);
    if (count) {
      uint32_t latest = sim.nextEpoch + (count - 1) * sim.options.intervalMs;
      sim.nextEpoch += count * sim.options.intervalMs;
      sim.counts.generated += count; sim.counts.busyDrops += count;
      // Missed ticks are discarded, including a tick delayed by blocking TX/HTTP.
      if (!sim.transmitting && radioReady && due(now, nextTx) && now - latest <= 50) {
        --sim.counts.busyDrops; newSimEpoch(sim.counts.generated);
      }
    }
  }
}
SimReport simReport() {
  SimReport r = sim.counts; r.elapsed = simElapsed(); r.active = sim.active;
  r.rate = sim.traffic.rate(r.elapsed); r.usefulRate = sim.useful.rate(r.elapsed);
  r.lastAge = r.complete ? r.elapsed - sim.lastComplete : SIM_UNKNOWN;
  r.meanInterval = r.complete > 1 ? (sim.lastComplete - sim.firstComplete) / (r.complete - 1) : SIM_UNKNOWN;
  r.maxGap = max(r.maxGap, r.complete ? r.elapsed - sim.lastComplete : r.elapsed);
  if (r.complete < 2) r.lastInterval = SIM_UNKNOWN;
  if (!r.complete) r.deliveryAge = SIM_UNKNOWN;
  return r;
}
void receiveSimulation(const Header &h, const uint8_t *data) {
  if (!sim.id || h.transfer != sim.id) return;
  if (h.type == SIM_REPORT) {
    if (h.length != sizeof(SimReport)) return;
    SimReport r; memcpy(&r, data, sizeof(r));
    if (r.active > 1 || r.elapsed > sim.duration || r.completeBytes > r.bytes ||
        r.complete > r.fragments || r.busyDrops > r.generated || r.sent > r.generated ||
        (sim.peerKnown && (r.elapsed < sim.peer.elapsed || r.complete < sim.peer.complete || r.generated < sim.peer.generated))) return;
    sim.peer = r; sim.peerKnown = true; sim.peerAt = millis();
    if (!r.active) { stopSimulation(false); sim.stopping = false; sim.finalReports.received(h.index); }
    return;
  }
  if (!sim.active || simSender() || h.type != SIM_DATA || h.length <= sizeof(SimFragment)) return;
  SimFragment f; memcpy(&f, data, sizeof(f));
  size_t n = h.length - sizeof(f), offset = size_t(h.index) * SIM_CHUNK;
  if (!f.epoch || f.generated < f.epoch || f.busyDrops > f.generated || f.size != simSize(sim.options.preset) ||
      f.ageMs >= sim.options.ttlMs || offset >= f.size || n != min(SIM_CHUNK, size_t(f.size - offset))) { ++sim.counts.bad; return; }
  uint32_t elapsed = simElapsed();
  // Source counters are monotonic even if its last radio fragment was lost.
  sim.counts.generated = max(sim.counts.generated, f.generated);
  sim.counts.busyDrops = max(sim.counts.busyDrops, f.busyDrops);
  sim.counts.expired = max(sim.counts.expired, f.expired);
  if (f.epoch < sim.seenEpoch || (f.epoch == sim.seenEpoch && !sim.assembly.active)) return;
  if (f.epoch > sim.seenEpoch) {
    if (sim.assembly.active) ++sim.counts.partial;
    sim.seenEpoch = f.epoch; sim.assembly.active = true; sim.assembly.epoch = f.epoch;
    sim.assembly.mask = 0; sim.assembly.size = f.size;
    sim.assembly.started = millis() - f.ageMs;
  }
  if (!sim.assembly.add(h.index, data + sizeof(f), n)) return;
  bool returning = sim.counts.fragments && due(millis(), sim.feedbackUntil + 3000);
  ++sim.counts.fragments; sim.counts.bytes += n; sim.traffic.add(elapsed, n);
  if (returning) sim.reportAt = 0;
  // Feedback is confined to the sender's mandatory off-air period.
  sim.feedbackUntil = millis() + offTime(packetAirtime(sizeof(Header) + h.length + TAG_SIZE)) - 40;
  if (!sim.assembly.complete()) return;
  uint8_t expected[SIM_MAX]; makeSimEpoch(expected, sim.options.preset, sim.id, f.epoch);
  if (!validRtcmFrames(sim.assembly.data, f.size) || memcmp(expected, sim.assembly.data, f.size)) ++sim.counts.bad;
  else {
    if (sim.counts.complete) {
      sim.counts.lastInterval = elapsed - sim.lastComplete;
      sim.counts.maxGap = max(sim.counts.maxGap, sim.counts.lastInterval);
    } else { sim.firstComplete = elapsed; sim.counts.firstEpoch = f.epoch; sim.counts.maxGap = elapsed; }
    ++sim.counts.complete; sim.counts.completeBytes += f.size; sim.counts.lastEpoch = f.epoch;
    sim.lastComplete = elapsed; sim.counts.deliveryAge = f.ageMs; sim.useful.add(elapsed, f.size);
  }
  sim.assembly.active = false;
}
bool sendSimReport() {
  SimReport r = simReport();
  uint16_t flags = sim.peerKnown && !sim.peer.active ? REPORT_FINAL_SEEN : 0;
  if (!transmitPacket(SIM_REPORT, sim.id, flags, reinterpret_cast<uint8_t *>(&r), sizeof(r))) return false;
  if (lastTransmitOK) {
    sim.reportAt = millis();
    if (!sim.active) sim.finalReports.transmitted();
  }
  return true;
}
bool tickSimulation() {
  uint32_t now = millis();
  if (sim.stopping && (!sim.stopAt || now - sim.stopAt >= 2000)) {
    if (transmitPacket(LAB_STOP, sim.id, 0, nullptr, 0)) sim.stopAt = millis();
    return true;
  }
  if (sim.active) {
    if (simSender()) {
      uint32_t firstAirMs = (packetAirtime(sizeof(Header) + MAX_FRAME_PAYLOAD + TAG_SIZE) + 999) / 1000;
      if (!sim.transmitting && simElapsed() + firstAirMs + 100 >= sim.duration) return true;
      if (!sim.transmitting && !sim.options.intervalMs) newSimEpoch(++sim.counts.generated);
      if (sim.transmitting) {
        size_t size = simSize(sim.options.preset), offset = size_t(sim.txIndex) * SIM_CHUNK;
        size_t n = min(SIM_CHUNK, size - offset);
        uint32_t airMs = (packetAirtime(sizeof(Header) + sizeof(SimFragment) + n + TAG_SIZE) + 999) / 1000;
        uint32_t age = millis() - sim.born + airMs;
        if (age >= sim.options.ttlMs || simElapsed() + airMs + 100 >= sim.duration) {
          ++sim.counts.expired; sim.transmitting = false; return true;
        }
        uint8_t data[MAX_FRAME_PAYLOAD];
        SimFragment f{sim.txEpoch, sim.counts.generated, sim.counts.busyDrops, sim.counts.expired, uint16_t(age), uint16_t(size)};
        memcpy(data, &f, sizeof(f)); memcpy(data + sizeof(f), sim.txData + offset, n);
        if (transmitPacket(SIM_DATA, sim.id, sim.txIndex, data, sizeof(f) + n)) {
          if (!lastTransmitOK) { ++sim.counts.txErrors; ++sim.counts.expired; sim.transmitting = false; }
          else if ((++sim.txIndex) * SIM_CHUNK >= size) { ++sim.counts.sent; sim.transmitting = false; }
        }
      }
    } else {
      uint32_t air = packetAirtime(sizeof(Header) + sizeof(SimReport) + TAG_SIZE);
      if (sim.counts.fragments && (!sim.reportAt || now - sim.reportAt >= reportInterval(air)) && feedbackFits(now, sim.feedbackUntil, air)) sendSimReport();
    }
    return true;
  }
  uint32_t reportEvery = max(3000U, benchCycle(packetAirtime(sizeof(Header) + sizeof(SimReport) + TAG_SIZE))) + uint32_t(NODE_ID) * 997;
  if (sim.id && sim.finalReports.needed() && !due(now, sim.reportUntil) && (!sim.reportAt || now - sim.reportAt >= reportEvery)) return sendSimReport();
  return false;
}
void appendSimReport(JsonObject o, const SimReport &r) {
  o["elapsedMs"] = r.elapsed; o["generated"] = r.generated; o["busyDrops"] = r.busyDrops;
  o["expired"] = r.expired; o["sent"] = r.sent; o["txErrors"] = r.txErrors;
  o["complete"] = r.complete; o["partial"] = r.partial; o["bad"] = r.bad; o["fragments"] = r.fragments;
  o["bytes"] = r.bytes; o["completeBytes"] = r.completeBytes; o["rate"] = r.rate; o["usefulRate"] = r.usefulRate;
  o["lastAgeMs"] = r.lastAge == SIM_UNKNOWN ? -1 : int32_t(r.lastAge);
  o["lastIntervalMs"] = r.lastInterval == SIM_UNKNOWN ? -1 : int32_t(r.lastInterval);
  o["meanIntervalMs"] = r.meanInterval == SIM_UNKNOWN ? -1 : int32_t(r.meanInterval);
  o["maxGapMs"] = r.maxGap; o["deliveryAgeMs"] = r.deliveryAge == SIM_UNKNOWN ? -1 : int32_t(r.deliveryAge);
  o["firstEpoch"] = r.firstEpoch; o["lastEpoch"] = r.lastEpoch; o["active"] = bool(r.active);
}
void appendSimulation(JsonDocument &doc) {
  if (!sim.id) return;
  auto s = doc["rtk"].to<JsonObject>();
  s["id"] = sim.id; s["active"] = sim.active; s["stopping"] = sim.stopping; s["sender"] = sim.sender;
  s["localSender"] = simSender(); s["profile"] = sim.profile; s["profileName"] = PROFILES[sim.profile].name;
  s["preset"] = sim.options.preset; s["epochBytes"] = simSize(sim.options.preset);
  s["intervalMs"] = sim.options.intervalMs; s["ttlMs"] = sim.options.ttlMs;
  s["reportIntervalMs"] = reportInterval(packetAirtime(sizeof(Header) + sizeof(SimReport) + TAG_SIZE));
  s["elapsedMs"] = simElapsed(); s["durationMs"] = sim.duration;
  s["peerKnown"] = sim.peerKnown; s["peerAgeMs"] = sim.peerKnown ? int32_t(millis() - sim.peerAt) : -1;
  s["assembling"] = sim.assembly.active; s["transmitting"] = sim.transmitting;
  appendSimReport(s["local"].to<JsonObject>(), simReport());
  if (sim.peerKnown) appendSimReport(s["peer"].to<JsonObject>(), sim.peer);
}
