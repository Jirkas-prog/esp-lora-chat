// Included after the chat transport: shares its authenticated frames and duty limiter.
#include "profile_trial.h"
uint8_t activeProfile = 0;
bool peerLab = false, peerSwitch = false;
ProfileTrial profileTrial;
uint32_t completedPlanId = 0;
String labMessage;
struct LabPlan {
  bool active = false, initiator = false, ready = false, committed = false, commitAcked = false;
  uint32_t id = 0, expires = 0, applyAt = 0, sendAt = 0;
  LabRequest request{};
  SimOptions simOptions{};
} plan;
struct LabReply {
  bool active = false;
  uint32_t id = 0;
  uint8_t result = 0;
  uint16_t index = 0;
} labReply;
struct Benchmark {
  bool active = false, peerKnown = false, stopping = false;
  uint8_t direction = 0, profile = 0;
  uint32_t id = 0, started = 0, duration = 0, endedElapsed = 0;
  uint32_t sent = 0, txErrors = 0, badPayloads = 0, cycle = 0, lastCycle = UINT32_MAX;
  uint32_t lastReport = 0, reportUntil = 0, peerAt = 0, stopUntil = 0, stopSent = 0;
  uint16_t reportJitter = 0;
  uint32_t feedbackUntil = 0;
  FinalReports finalReports;
  BenchRx rx;
  BenchReport peer{};
} bench;

#include "rtk_sim_runtime.h"

bool labBusy() { return profileTrial.active || plan.active || bench.active || bench.stopping || sim.active || sim.stopping; }
uint32_t packetAirtime(size_t size) {
  // RadioLib 7.1.2 counts only GFSK payload; include the physical framing too.
  return loraProfile(activeProfile) ? radio.getTimeOnAir(size) : gfskAirtimeUs(size, PROFILES[activeProfile].kbps);
}
bool chatBusy() { return !jobs.empty() || incoming.active || uploading || otaInProgress || rebootAt; }
uint32_t benchElapsed() {
  return bench.active ? min(uint32_t(millis() - bench.started), bench.duration) : bench.endedElapsed;
}
void saveProfile(uint8_t profile) {
  Preferences settings;
  if (settings.begin("lorachat", false)) {
    if (settings.getUChar("profile", 255) != profile) settings.putUChar("profile", profile);
    settings.end();
  }
}
bool configureRadio(uint8_t profile, bool resetLink = true) {
  if (profile >= PROFILE_COUNT) return false;
  radioQuality.setProfile(profile,millis());
  const auto &p = PROFILES[profile];
  radio.clearDio1Action(); radioIRQ = false;
  if (loraProfile(profile))
    radioError = radio.begin(869.525, p.rxBandwidth, p.sf, 5,
      RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, p.preamble, 1.8);
  else {
    radioError = radio.beginFSK(869.525, p.kbps, p.deviation, p.rxBandwidth, 10, p.preamble, 1.8);
    uint8_t sync[] = {0x2d, 0xd4, 0x37, 0xc9};
    if (!radioError) radioError = radio.setSyncWord(sync, sizeof(sync));
    if (!radioError) radioError = radio.setDataShaping(RADIOLIB_SHAPING_0_5);
    if (!radioError) radioError = radio.setWhitening(true);
  }
  if (!radioError) radioError = radio.setDio2AsRfSwitch(true);
  if (!radioError) radioError = radio.setCRC(2);
  if (!radioError) {
    radio.setDio1Action(onRadioIRQ);
    radioError = startRadioReceive();
  }
  radioReady = radioError == RADIOLIB_ERR_NONE;
  activeProfile = profile;
  if (resetLink) {
    radioQuality.peerBoot=0; radioQuality.peerCounter=0;
    peerLink.reset(millis(), randomChallenge(), esp_random(), NODE_ID, packetAirtime(sizeof(Header) + 16 + TAG_SIZE));
    peerLab = peerSim = peerSwitch = false; ack.active = false;
  }
  if (radioReady) radioFailures = 0;
  else { radioRetryAt = millis() + (1000U << min(uint8_t(5), radioFailures)); if (radioFailures < 5) ++radioFailures; }
  // Never reset txCounter or nextTx: preserve GCM nonce uniqueness and airtime debt.
  Serial.printf("PROFILE %u radio=%d\n", profile, radioError);
  return radioReady;
}
void stopBenchmark(bool notifyPeer) {
  if (!bench.id) return;
  if (bench.active) {
    bench.endedElapsed = benchElapsed(); bench.active = false;
    bench.reportUntil = millis() + 20000; bench.lastReport = 0;
    labMessage = "Test ukončen. Čekám na výsledky druhé desky.";
  }
  if (notifyPeer) { bench.stopping = true; bench.stopUntil = millis() + 20000; bench.stopSent = 0; }
}
void rescueRadio() {
  profileTrial.active = false;
  stopSimulation(false); sim.stopping = false; sim.reportUntil = 0;
  stopBenchmark(false); bench.stopping = false; bench.reportUntil = 0;
  plan.active = labReply.active = false;
  if (incoming.active) resetIncoming();
  if (!jobs.empty()) { jobs.front().waiting = false; jobs.front().index = BEGIN_INDEX; jobs.front().attempts = 0; }
  configureRadio(0);
  saveProfile(0);
  labMessage = "Obnovena základní LoRa. Při RTK testu mimo dosah obnovte i druhou desku ručně; jinak se vrátí po skončení testu.";
}
String startLab(const LabRequest &request, const SimOptions &options = {}) {
  if (!validLabRequest(request) || (request.action == START_SIM && !validSimOptions(options))) return "Neplatný režim nebo parametry testu.";
  if (!radioReady || !peerSession || millis() - lastPeer > LINK_STALE_MS || !peerLab)
    return "Počkejte na spojení. Obě desky musí mít firmware 1.2.0 nebo novější.";
  if (labBusy() || chatBusy()) return "Nejdřív dokončete přenos, test nebo přepnutí.";
  if (request.action == CHANGE_PROFILE && !peerSwitch) return "Pro bezpečné přepínání aktualizujte obě desky na 1.5.0.";
  if (request.action == START_SIM && !peerSim) return "RTK simulace vyžaduje firmware 1.3.0 na obou deskách.";
  if (request.action != CHANGE_PROFILE && request.profile != activeProfile) return "Nejdřív přepněte rádiový režim.";
  if (request.action == CHANGE_PROFILE && request.profile == activeProfile) return "Tento režim už je aktivní.";
  plan = LabPlan(); plan.active = plan.initiator = true; plan.request = request; plan.simOptions = options;
  do { plan.id = esp_random(); } while (!plan.id || plan.id == completedPlanId);
  plan.sendAt = millis(); plan.expires = millis() + 30000;
  labMessage = "Čekám na souhlas druhé desky…";
  return "";
}
BenchReport currentBenchReport() {
  return BenchReport{benchElapsed(), bench.sent, bench.rx.received, bench.rx.expected,
    bench.rx.rate(benchElapsed()), bench.txErrors, uint8_t(bench.active)};
}
bool acceptBenchReport(const uint8_t *data, uint16_t flags = 0) {
  BenchReport r; memcpy(&r, data, sizeof(r));
  if (r.active > 1 || r.elapsed > 600000 || r.sent > 65535 || r.expected > 65535 ||
      r.received > r.expected || r.rate > 50000) return false;
  if (bench.peerKnown && (r.elapsed < bench.peer.elapsed || r.received < bench.peer.received || r.sent < bench.peer.sent)) return false;
  bench.peer = r; bench.peerKnown = true; bench.peerAt = millis();
  if (!r.active) {
    stopBenchmark(false); bench.stopping = false;
    bench.finalReports.received(flags);
    labMessage = "Test ukončen. Přijaty výsledky druhé desky.";
  }
  return true;
}
void receiveLab(const Header &h, const uint8_t *data) {
  if (h.type == PROFILE_PROOF) {
    if (h.length != sizeof(ProfileProof) || !peerSwitch || peerSession != profileTrial.peerBoot) return;
    ProfileProof proof; memcpy(&proof, data, sizeof(proof));
    profileTrial.receive(h.transfer, proof, millis());
  } else if (h.type == LAB_REQUEST) {
    if ((h.length != sizeof(LabRequest) && h.length != sizeof(SimRequest)) || !h.transfer || h.transfer == completedPlanId) return;
    LabRequest r; memcpy(&r, data, sizeof(r));
    SimOptions options{};
    if (h.length == sizeof(SimRequest)) memcpy(&options, data + sizeof(r), sizeof(options));
    uint8_t result = !validLabRequest(r) ? INVALID : chat::OK;
    if (r.action == START_SIM ? (h.length != sizeof(SimRequest) || !validSimOptions(options)) : h.length != sizeof(LabRequest)) result = INVALID;
    if (r.action != CHANGE_PROFILE && r.profile != activeProfile) result = INVALID;
    if (r.action == CHANGE_PROFILE && (!peerSwitch || r.profile == activeProfile)) result = INVALID;
    if (profileTrial.active || chatBusy() || bench.active || bench.stopping || sim.active || sim.stopping || (plan.active && plan.id != h.transfer)) result = chat::BUSY;
    if (result == chat::OK && !plan.active) {
      plan = LabPlan(); plan.active = true; plan.id = h.transfer; plan.request = r; plan.simOptions = options;
      plan.expires = millis() + 35000; labMessage = "Druhá deska připravuje změnu…";
    }
    labReply.active = true; labReply.id = h.transfer; labReply.result = result; labReply.index = 0;
  } else if (h.type == LAB_REPLY) {
    if (h.length != 1 || !plan.active || !plan.initiator || h.transfer != plan.id || h.index > 1) return;
    if (h.index == 1) { if (data[0] == chat::OK && plan.committed) plan.commitAcked = true; return; }
    if (data[0] != chat::OK) {
      plan.active = false; labMessage = "Druhá deska odmítla požadavek: je zaneprázdněná nebo má jiný režim.";
    } else if (!plan.ready) {
      plan.ready = true; plan.applyAt = millis() + 15000;
      plan.expires = plan.applyAt + 1000; plan.sendAt = millis();
      labMessage = "Potvrzeno. Společný start za několik sekund…";
    }
  } else if (h.type == LAB_COMMIT) {
    if (h.length != 4 || !plan.active || plan.initiator || h.transfer != plan.id) return;
    uint32_t delayMs; memcpy(&delayMs, data, 4);
    if (delayMs < 500 || delayMs > 16000) return;
    plan.ready = plan.committed = true; plan.applyAt = millis() + delayMs;
    plan.expires = plan.applyAt + 1000; labMessage = "Potvrzeno. Společný start za několik sekund…";
    labReply.active = true; labReply.id = h.transfer; labReply.result = chat::OK; labReply.index = 1;
  } else if (h.type == BENCH_DATA) {
    if (!bench.active || h.transfer != bench.id || !(bench.direction & (1 << (PEER - 1))) || h.length != MAX_FRAME_PAYLOAD) return;
    for (size_t i = 0; i < BENCH_BYTES; ++i) if (data[sizeof(BenchReport) + i] != testByte(h.transfer, h.index, i)) { ++bench.badPayloads; return; }
    if (!acceptBenchReport(data)) return;
    bool returning = bench.rx.received && due(millis(), bench.feedbackUntil + 3000);
    bench.rx.add(h.index, benchElapsed());
    if (returning) bench.lastReport = 0;
    bench.feedbackUntil = millis() + offTime(packetAirtime(sizeof(Header) + h.length + TAG_SIZE)) - 40;
  } else if (h.type == BENCH_REPORT) {
    if (!bench.id || h.transfer != bench.id || h.length != sizeof(BenchReport)) return;
    acceptBenchReport(data, h.index);
  } else if (h.type == SIM_DATA || h.type == SIM_REPORT) {
    receiveSimulation(h, data);
  } else if (h.type == LAB_STOP && !h.length) {
    if (plan.active && plan.id == h.transfer) { plan.active = false; labMessage = "Příprava zrušena druhou deskou."; }
    if (bench.id == h.transfer) { stopBenchmark(false); bench.lastReport = 0; bench.reportUntil = millis() + 20000; }
    if (sim.id == h.transfer) { stopSimulation(false); sim.reportAt = 0; sim.reportUntil = millis() + 20000; }
  }
}
void rollbackProfile(const char *message) {
  uint8_t previous = profileTrial.previous;
  profileTrial.active = false; labReply.active = false;
  configureRadio(previous);
  labMessage = message;
}
void maintainLab() {
  uint32_t now = millis();
  if (!radioReady && radioError != -999 && due(now, radioRetryAt)) {
    if (configureRadio(activeProfile, false)) ++radioRecoveries;
  }
  if (profileTrial.active) {
    if (peerSession && peerSession != profileTrial.peerBoot)
      rollbackProfile("Druhá deska se restartovala. Vracím původní režim.");
    else if (due(now, profileTrial.until)) {
      if (radioReady && profileTrial.success(now, peerSession)) {
        profileTrial.active = false; saveProfile(activeProfile);
        labMessage = "Nový režim ověřen v obou směrech a uložen.";
      } else rollbackProfile("Nový režim se nepotvrdil. Vracím původní režim.");
    }
  }
  if (plan.active && plan.ready && due(now, plan.applyAt)) {
    LabRequest r = plan.request; uint32_t id = plan.id;
    bool apply = plan.committed; plan.active = false; completedPlanId = id;
    if (!apply) { labMessage = "Přepnutí nebylo odesláno. Zkuste znovu."; return; }
    if (r.action == CHANGE_PROFILE) {
      bench = Benchmark(); sim.reportUntil = 0; labReply.active = false;
      profileTrial.begin(now, id, activeProfile, r.profile, peerSession);
      if (!configureRadio(r.profile)) rollbackProfile("Rádio odmítlo nový režim. Vracím původní.");
      else labMessage = "Obě desky zkoušejí nový režim. Ověřuji obousměrné spojení…";
    } else if (r.action == START_SIM) {
      bench = Benchmark(); startSimulation(r, plan.simOptions, id);
    } else {
      resetSimulation();
      bench = Benchmark(); bench.active = true; bench.id = id;
      bench.direction = r.direction; bench.profile = activeProfile;
      bench.started = now; bench.duration = uint32_t(r.seconds) * 1000;
      bench.cycle = benchCycle(packetAirtime(sizeof(Header) + MAX_FRAME_PAYLOAD + TAG_SIZE));
      labMessage = "Probíhá test propustnosti.";
    }
  } else if (plan.active && due(now, plan.expires)) {
    plan.active = false; labMessage = "Druhá deska nepotvrdila požadavek. Režim se nezměnil.";
  }
  if (bench.active && due(now, bench.started + bench.duration)) {
    stopBenchmark(false); labMessage = "Test dokončen. Čekám na závěrečné výsledky druhé desky.";
  }
  if (bench.stopping && due(now, bench.stopUntil)) { bench.stopping = false; labMessage = "Stop nebyl potvrzen. Vzdálený test skončí nejpozději po zvolené době."; }
  maintainSimulation();
  now = millis();
  if (activeProfile && !labBusy() && peerLink.searching && now - peerLink.searchSince > 20000) {
    rescueRadio(); labMessage = "Druhá deska neodpověděla na hledání. Obnovena základní LoRa.";
  }
}
bool sendBenchReport() {
  BenchReport r = currentBenchReport();
  uint16_t flags = bench.peerKnown && !bench.peer.active ? REPORT_FINAL_SEEN : 0;
  if (!transmitPacket(BENCH_REPORT, bench.id, flags, reinterpret_cast<uint8_t *>(&r), sizeof(r))) return false;
  if (lastTransmitOK) {
    bench.lastReport = millis(); bench.reportJitter = esp_random() % 1000;
    if (!bench.active) bench.finalReports.transmitted();
  }
  return true;
}
bool tickLab() {
  uint32_t now = millis();
  if (labReply.active) {
    if (transmitPacket(LAB_REPLY, labReply.id, labReply.index, &labReply.result, 1) && lastTransmitOK) labReply.active = false;
    return true;
  }
  if (profileTrial.active) {
    if (peerSession && peerSwitch && due(now, profileTrial.sendAt)) {
      ProfileProof proof{profileTrial.previous, profileTrial.target, uint8_t(profileTrial.seenPeer)};
      if (transmitPacket(PROFILE_PROOF, profileTrial.id, 0, reinterpret_cast<uint8_t *>(&proof), sizeof(proof))) {
        if (lastTransmitOK && proof.seenPeer) profileTrial.echoed = true;
        profileTrial.sendAt = millis() + 2200 + NODE_ID * 400 + esp_random() % 500;
      }
    }
    return true;
  }
  if (plan.active) {
    if (plan.initiator && due(now, plan.sendAt)) {
      if (!plan.ready) {
        SimRequest request{plan.request, plan.simOptions};
        size_t size = plan.request.action == START_SIM ? sizeof(request) : sizeof(plan.request);
        if (transmitPacket(LAB_REQUEST, plan.id, 0, reinterpret_cast<uint8_t *>(&request), size)) plan.sendAt = millis() + 2500;
      } else if (!plan.commitAcked && int32_t(plan.applyAt - now) > 4500) {
        uint32_t airMs = (packetAirtime(sizeof(Header) + 4 + TAG_SIZE) + 999) / 1000;
        uint32_t delayMs = plan.applyAt - now - airMs;
        if (transmitPacket(LAB_COMMIT, plan.id, 0, reinterpret_cast<uint8_t *>(&delayMs), 4)) {
          if (lastTransmitOK) plan.committed = true;
          plan.sendAt = millis() + 2500;
        }
      }
    }
    return true;
  }
  if (tickSimulation()) return true;
  if (bench.stopping && (!bench.stopSent || now - bench.stopSent >= 2000)) {
    if (transmitPacket(LAB_STOP, bench.id, 0, nullptr, 0)) bench.stopSent = millis();
    return true;
  }
  if (bench.active) {
    bool sendData = bench.direction & (1 << (NODE_ID - 1));
    if (sendData) {
      uint32_t elapsed = benchElapsed(), cycle = elapsed / bench.cycle;
      if (bench.direction == 3 && (!benchSlot(elapsed, bench.cycle, NODE_ID) || cycle == bench.lastCycle)) return true;
      {
        uint8_t data[MAX_FRAME_PAYLOAD];
        BenchReport r = currentBenchReport(); memcpy(data, &r, sizeof(r));
        for (size_t i = 0; i < BENCH_BYTES; ++i) data[sizeof(r) + i] = testByte(bench.id, bench.sent, i);
        if (transmitPacket(BENCH_DATA, bench.id, bench.sent, data, sizeof(data))) {
          if (lastTransmitOK) ++bench.sent; else ++bench.txErrors;
          bench.lastCycle = cycle;
        }
      }
    } else {
      uint32_t air = packetAirtime(sizeof(Header) + sizeof(BenchReport) + TAG_SIZE);
      if (bench.rx.received && (!bench.lastReport || now - bench.lastReport >= reportInterval(air)) &&
          feedbackFits(now, bench.feedbackUntil, air)) sendBenchReport();
    }
    return true;
  }
  if (bench.id && bench.finalReports.needed() && !due(now, bench.reportUntil) &&
      (!bench.lastReport || now - bench.lastReport >= 2500U + bench.reportJitter + NODE_ID * 497U)) return sendBenchReport();
  return false;
}
void encodeBenchDirection(JsonObject out, uint8_t sender) {
  bool localReceiver = sender != NODE_ID;
  bool known = localReceiver || bench.peerKnown;
  uint32_t received = localReceiver ? bench.rx.received : bench.peer.received;
  uint32_t expected = localReceiver ? bench.rx.expected : bench.peer.expected;
  uint32_t elapsed = localReceiver ? benchElapsed() : bench.peer.elapsed;
  bool final = !bench.active && bench.peerKnown && !bench.peer.active;
  if (final) expected = max(expected, localReceiver ? bench.peer.sent : bench.sent);
  uint32_t age = localReceiver ? 0 : millis() - bench.peerAt;
  uint32_t liveInterval = reportInterval(packetAirtime(sizeof(Header) + sizeof(BenchReport) + TAG_SIZE));
  bool fresh = known && (localReceiver || age <= max(liveInterval * 2 + 1000, bench.cycle * 3));
  out["enabled"] = bool(bench.direction & (1 << (sender - 1))); out["known"] = known;
  out["fresh"] = fresh; out["final"] = final; out["reportAgeMs"] = known ? int32_t(age) : -1;
  out["received"] = received; out["expected"] = expected; out["bytes"] = received * BENCH_BYTES;
  out["rate"] = localReceiver ? bench.rx.rate(elapsed) : bench.peer.rate;
  out["average"] = elapsed ? uint64_t(received) * BENCH_BYTES * 1000 / elapsed : 0;
  out["lost"] = expected >= received ? expected - received : 0;
  out["lossPct"] = expected ? 100.0 * uint32_t(out["lost"]) / expected : 0.0;
}
void appendLabState(JsonDocument &doc) {
  auto r = doc["radio"].to<JsonObject>();
  r["profile"] = activeProfile; r["name"] = PROFILES[activeProfile].name;
  r["modulation"] = loraProfile(activeProfile) ? "LoRa" : "GFSK";
  r["powerDbm"] = 10; r["frequencyMHz"] = 869.525; r["dutyPercent"] = 10;
  r["peerCapable"] = peerLab; r["busy"] = labBusy(); r["message"] = labMessage;
  r["peerSwitchCapable"] = peerSwitch;
  r["peerSimCapable"] = peerSim; r["profileLocked"] = sim.active || sim.stopping;
  r["switching"] = plan.active || profileTrial.active; r["pendingProfile"] = plan.active ? plan.request.profile : activeProfile;
  r["verifying"] = profileTrial.active;
  r["verifyRemainingMs"] = profileTrial.active && !due(millis(), profileTrial.until) ? profileTrial.until - millis() : 0;
  r["countdownMs"] = plan.active && plan.ready && !due(millis(), plan.applyAt) ? plan.applyAt - millis() : 0;
  auto list = r["profiles"].to<JsonArray>();
  for (uint8_t i = 0; i < PROFILE_COUNT; ++i) {
    auto p = list.add<JsonObject>(); p["id"] = i; p["name"] = PROFILES[i].name;
    p["hint"] = PROFILES[i].hint; p["modulation"] = loraProfile(i) ? "LoRa" : "GFSK";
    p["rawKbps"] = loraProfile(i) ? (125.0 * PROFILES[i].sf * 0.8 / (1U << PROFILES[i].sf)) : PROFILES[i].kbps;
  }
  appendSimulation(doc);
  if (!bench.id) return;
  auto b = doc["benchmark"].to<JsonObject>();
  b["id"] = bench.id; b["active"] = bench.active; b["stopping"] = bench.stopping;
  b["profile"] = bench.profile; b["profileName"] = PROFILES[bench.profile].name;
  b["direction"] = bench.direction; b["elapsedMs"] = benchElapsed(); b["durationMs"] = bench.duration;
  b["sent"] = bench.sent; b["txErrors"] = bench.txErrors; b["badPayloads"] = bench.badPayloads;
  encodeBenchDirection(b["aToB"].to<JsonObject>(), 1);
  encodeBenchDirection(b["bToA"].to<JsonObject>(), 2);
}
