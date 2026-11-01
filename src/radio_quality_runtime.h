// Included after radio_lab.h. Snapshot stays local to the AP, never goes over LoRa.
void makeQuality(JsonDocument &doc) {
  const auto &q=radioQuality; uint32_t now=millis();
  doc["node"]=NODE_NAME; doc["version"]=FIRMWARE_VERSION; doc["boot"]=hex64(sessionId);
  doc["generation"]=q.generation; doc["profile"]=activeProfile; doc["profileName"]=PROFILES[activeProfile].name;
  doc["lora"]=loraProfile(activeProfile); doc["elapsedMs"]=uint32_t(now-q.since);
  doc["radioReady"]=radioReady; doc["online"]=peerSession && now-lastPeer<LINK_OFFLINE_MS;
  doc["signalAgeMs"]=q.hasSignal ? int32_t(now-q.lastSignalAt) : -1;
  if(q.hasSignal)doc["rssi"]=q.lastRssi; else doc["rssi"]=nullptr;
  if(q.hasSnr)doc["snr"]=q.lastSnr; else doc["snr"]=nullptr;
  if(q.hasChannel)doc["channelRssi"]=q.channelRssi; else doc["channelRssi"]=nullptr;
  doc["channelAgeMs"]=q.hasChannel ? int32_t(now-q.channelAt) : -1;
  auto counts=doc["counts"].to<JsonObject>();
  const char *names[]={"accepted","crc","header","format","auth","session","handshake","io"};
  for(size_t i=0;i<RX_OUTCOMES;++i)counts[names[i]]=q.counts[i];
  counts["txOK"]=q.txOK; counts["txErrors"]=q.txErrors; counts["txBytes"]=q.txBytes;
  counts["retries"]=q.retries; counts["recovered"]=q.recovered;
  counts["duplicates"]=q.duplicates; counts["delivered"]=q.delivered; counts["gaps"]=q.gaps;
  doc["fecCorrected"]=nullptr; // Not exposed by this radio; do not infer from CRC success.
  auto samples=doc["samples"].to<JsonArray>();
  uint32_t bytes=0, accepted=0, rejected=0;
  for(const auto &b:q.bins) {
    if(!q.recent(b,now))continue;
    uint32_t age=now-b.atMs;
    if(age<10000){bytes+=b.bytes;accepted+=b.accepted;rejected+=b.rejected;}
    auto s=samples.add<JsonArray>(); s.add(age); s.add(b.accepted); s.add(b.rejected); s.add(b.bytes);
    if(b.signalSamples)s.add(b.rssiSum/b.signalSamples);else s.add(nullptr);
    if(b.snrSamples)s.add(b.snrSum/b.snrSamples);else s.add(nullptr);
    if(b.channelSamples)s.add(b.channelMin);else s.add(nullptr);
  }
  auto window=doc["window"].to<JsonObject>();
  uint32_t span=min(uint32_t(10000),uint32_t(now-q.since));
  window["ms"]=span; window["accepted"]=accepted; window["rejected"]=rejected;
  window["bytesPerSecond"]=span ? bytes*1000.0/span : 0;
  if(accepted+rejected)window["rejectedPercent"]=100.0*rejected/(accepted+rejected);
  else window["rejectedPercent"]=nullptr;
}
