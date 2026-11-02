#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <RadioLib.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <Preferences.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include <deque>
#include "protocol.h"
#include "lab_protocol.h"
#include "rtk_sim.h"
#include "radio_quality.h"
#include "ota_package.h"
#include "secrets.h"
#include "web_gz.h"

using namespace chat;
constexpr uint8_t PEER = 3 - NODE_ID;
constexpr size_t HISTORY_LIMIT = 32, QUEUE_LIMIT = 8;
constexpr uint32_t ACK_TIMEOUT = 10000, RX_TIMEOUT = 120000;
const char *NODE_NAME = NODE_ID == 1 ? "A" : "B";
const char *FIRMWARE_VERSION = "1.6.1";
WebServer web(80);
DNSServer dns;
SX1262 radio = new Module(41, 39, 42, 40);
mbedtls_gcm_context cipher;
uint64_t sessionId = 0;
PeerLink peerLink;
const uint64_t &peerSession = peerLink.session;
const uint32_t &lastPeer = peerLink.lastRx;
uint32_t txCounter = 0, nextTx = 0;
uint32_t txPackets = 0, rxPackets = 0, rejectedPackets = 0, retransmits = 0;
uint32_t servicePackets = 0, dataPackets = 0, helloPackets = 0;
uint64_t serviceAirtimeUs = 0, dataAirtimeUs = 0;
uint32_t radioRetryAt = 0, radioRecoveries = 0;
uint8_t radioFailures = 0;
volatile bool radioIRQ = false;
bool radioReady = false, filesystemReady = false;
bool lastTransmitOK = false;
bool labBusy();
uint32_t packetAirtime(size_t size);
int radioError = 0;
float lastRSSI = 0, lastSNR = 0;
RadioQuality radioQuality{};
String csrfToken, serialLine;

struct Message {
  String id, author, name, text, mime, status, error;
  uint32_t size = 0;
  bool outgoing = false, file = false;
};
struct Job {
  String id;
  uint32_t transfer = 0, deadline = 0;
  uint16_t index = BEGIN_INDEX;
  uint8_t attempts = 0, restarts = 0;
  bool waiting = false;
};
struct Incoming {
  bool active = false;
  uint64_t session = 0;
  uint32_t transfer = 0, touched = 0;
  uint16_t expected = 0;
  uint8_t hash[32] = {};
  Message message;
  File stream;
} incoming;
struct PendingAck {
  bool active = false;
  uint32_t transfer = 0;
  uint16_t index = 0;
  uint8_t result = chat::OK;
} ack;
std::deque<Message> messages;
// ponytail: stop-and-wait with eight queued messages; windowed ARQ only if throughput needs it.
std::deque<Job> jobs;
File uploadFile;
String uploadError, uploadName, uploadAuthor;
uint32_t uploadSize = 0, uploadTouched = 0;
bool uploading = false, uploadFinished = false;
OtaHeader otaHeader;
mbedtls_sha256_context otaHash;
size_t otaHeaderBytes = 0;
uint32_t otaBytes = 0, otaTouched = 0, rebootAt = 0;
uint8_t otaFileCount = 0;
bool otaInProgress = false, otaFinished = false;
String otaError;

void IRAM_ATTR onRadioIRQ() { radioIRQ = true; }
uint64_t randomChallenge() {
  uint64_t value;
  do { esp_fill_random(&value, sizeof(value)); } while (!value);
  return value;
}
int startRadioReceive() {
  // Also surface damaged headers/CRC, even when RX_DONE never arrives.
  return radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF, RADIOLIB_IRQ_RX_DEFAULT_FLAGS,
    RADIOLIB_IRQ_RX_DEFAULT_MASK | (1UL << RADIOLIB_IRQ_CRC_ERR) | (1UL << RADIOLIB_IRQ_HEADER_ERR), 0);
}
void restartReceive() {
  int state = startRadioReceive();
  if (state != RADIOLIB_ERR_NONE) { radioError = state; radioReady = false; radioRetryAt = millis() + 1000; }
}
String hex64(uint64_t value) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%08x%08x", uint32_t(value >> 32), uint32_t(value));
  return buffer;
}
String digestHex(const uint8_t *digest) {
  const char *digits = "0123456789abcdef";
  String result; result.reserve(64);
  for (size_t i = 0; i < 32; ++i) { result += digits[digest[i] >> 4]; result += digits[digest[i] & 15]; }
  return result;
}
String messageId(uint64_t session, uint32_t transfer, uint8_t sender) {
  char buffer[10];
  snprintf(buffer, sizeof(buffer), "%08x", transfer);
  return String(sender) + "-" + hex64(session) + "-" + buffer;
}
String dataPath(const String &id) { return "/m-" + id + ".bin"; }
bool validId(const String &id) {
  if (id.length() != 27 || id[1] != '-' || id[18] != '-') return false;
  for (size_t i = 0; i < id.length(); ++i)
    if (i != 1 && i != 18 && !isxdigit(static_cast<unsigned char>(id[i]))) return false;
  return id[0] == '1' || id[0] == '2';
}
String cleanLabel(String value, size_t limit) {
  value.replace("/", "_"); value.replace("\\", "_");
  for (size_t i = 0; i < value.length(); ++i)
    if (static_cast<uint8_t>(value[i]) < 32 || value[i] == '"') value.setCharAt(i, '_');
  if (value.length() > limit) {
    size_t end = limit;
    while (end && (static_cast<uint8_t>(value[end]) & 0xc0) == 0x80) --end;
    value.remove(end);
  }
  value.trim();
  return value;
}
Message *findMessage(const String &id) {
  for (auto &m : messages) if (m.id == id) return &m;
  return nullptr;
}
bool pending(const Message &m) { return m.status == "queued" || m.status == "sending"; }
void encodeMessage(JsonObject obj, const Message &m) {
  obj["id"] = m.id; obj["author"] = m.author; obj["name"] = m.name;
  obj["text"] = m.text; obj["mime"] = m.mime; obj["status"] = m.status;
  obj["error"] = m.error; obj["size"] = m.size;
  obj["outgoing"] = m.outgoing; obj["file"] = m.file;
}
void saveHistory() {
  JsonDocument doc;
  auto list = doc.to<JsonArray>();
  for (const auto &m : messages) encodeMessage(list.add<JsonObject>(), m);
  File file = LittleFS.open("/history.tmp", "w");
  if (!file) return;
  size_t written = serializeJson(doc, file);
  file.close();
  if (written == measureJson(doc)) LittleFS.rename("/history.tmp", "/history.json");
}
void pruneHistory() {
  while (messages.size() >= HISTORY_LIMIT) {
    auto it = messages.begin();
    while (it != messages.end() && pending(*it)) ++it;
    if (it == messages.end()) break;
    LittleFS.remove(dataPath(it->id));
    messages.erase(it);
  }
}
void loadHistory() {
  File file = LittleFS.open("/history.json", "r");
  if (!file) return;
  JsonDocument doc;
  if (deserializeJson(doc, file)) return;
  for (JsonObject obj : doc.as<JsonArray>()) {
    Message m;
    m.id = obj["id"].as<String>();
    if (!validId(m.id) || messages.size() >= HISTORY_LIMIT) continue;
    m.author = obj["author"].as<String>(); m.name = obj["name"].as<String>();
    m.text = obj["text"].as<String>(); m.mime = obj["mime"].as<String>();
    m.status = obj["status"].as<String>(); m.error = obj["error"].as<String>();
    m.size = obj["size"] | 0U; m.outgoing = obj["outgoing"] | false;
    m.file = obj["file"] | false;
    if (pending(m)) { m.status = "failed"; m.error = "Přenos přerušil restart desky; odešlete znovu."; }
    messages.push_back(m);
  }
}
bool hashFile(const String &path, uint8_t digest[32]) {
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  uint8_t buffer[512];
  while (file.available()) {
    size_t n = file.read(buffer, sizeof(buffer));
    if (!n) { mbedtls_sha256_free(&ctx); return false; }
    mbedtls_sha256_update_ret(&ctx, buffer, n);
    yield();
  }
  mbedtls_sha256_finish_ret(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  return true;
}
String sniffMime(const String &path) {
  File file = LittleFS.open(path, "r");
  uint8_t b[12] = {};
  size_t n = file.read(b, sizeof(b));
  if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
  if (n >= 3 && b[0] == 255 && b[1] == 216 && b[2] == 255) return "image/jpeg";
  if (n == 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) return "image/webp";
  return "application/octet-stream";
}
bool storageAvailable() {
  return filesystemReady && LittleFS.totalBytes() - LittleFS.usedBytes() >= MAX_FILE * 2 + 16384;
}
void failJob(const String &reason) {
  if (jobs.empty()) return;
  Message *m = findMessage(jobs.front().id);
  if (m) { m->status = "failed"; m->error = reason; }
  jobs.pop_front();
  saveHistory();
}
String enqueue(const String &tempPath, Message m) {
  if (jobs.size() >= QUEUE_LIMIT) return "Fronta je plná (8 zpráv).";
  uint32_t id = esp_random();
  while (!id || findMessage(messageId(sessionId, id, NODE_ID))) id = esp_random();
  m.id = messageId(sessionId, id, NODE_ID);
  m.outgoing = true; m.status = "queued";
  if (!LittleFS.rename(tempPath, dataPath(m.id))) return "Nelze uložit zprávu.";
  if (m.file) m.mime = sniffMime(dataPath(m.id));
  pruneHistory();
  messages.push_back(m);
  Job job; job.id = m.id; job.transfer = id;
  jobs.push_back(job);
  saveHistory();
  return "";
}
String queueText(String text, String author) {
  if (labBusy()) return "Nejdřív dokončete test nebo přepnutí rádia.";
  if (otaInProgress || rebootAt) return "Probíhá aktualizace firmwaru.";
  if (!radioReady || !filesystemReady) return "Rádio nebo úložiště není připraveno.";
  if (text.isEmpty() || text.length() > MAX_TEXT) return "Zpráva musí mít 1 až 2048 bajtů UTF-8.";
  if (!storageAvailable()) return "Úložiště je plné. Smažte historii.";
  File file = LittleFS.open("/text.tmp", "w");
  if (!file) return "Nelze otevřít úložiště.";
  size_t n = file.print(text); file.close();
  if (n != text.length()) return "Úložiště nemá místo.";
  Message m; m.text = text; m.size = n;
  m.author = cleanLabel(author, 32);
  if (m.author.isEmpty()) m.author = String("Stanice ") + NODE_NAME;
  return enqueue("/text.tmp", m);
}

bool transmitPacket(uint8_t type, uint32_t transfer, uint16_t index,
                    const uint8_t *payload, size_t length) {
  lastTransmitOK = false;
  if (!radioReady || !due(millis(), nextTx) || length > MAX_FRAME_PAYLOAD) return false;
  if (txCounter == UINT32_MAX) { radioReady = false; radioError = -999; return false; }
  Header h{0xC74A, 1, type, NODE_ID, PEER, sessionId, ++txCounter, transfer, index, uint8_t(length)};
  uint8_t frame[255], nonce[12];
  memcpy(nonce, &h.session, 8); memcpy(nonce + 8, &h.counter, 4);
  memcpy(frame, &h, sizeof(h));
  if (mbedtls_gcm_crypt_and_tag(&cipher, MBEDTLS_GCM_ENCRYPT, length, nonce, 12,
      frame, sizeof(h), payload, frame + sizeof(h), TAG_SIZE,
      frame + sizeof(h) + length) != 0) return false;
  size_t size = sizeof(h) + length + TAG_SIZE;
  uint32_t airtime = packetAirtime(size);
  if (type == DATA || type == BENCH_DATA || type == SIM_DATA) { ++dataPackets; dataAirtimeUs += airtime; }
  else { ++servicePackets; serviceAirtimeUs += airtime; }
  if (type == HELLO) ++helloPackets;
  radio.clearDio1Action();
  radioIRQ = false;
  int state = radio.transmit(frame, size);
  // Account for attempted transmissions too, including errors.
  nextTx = millis() + offTime(airtime);
  radio.setDio1Action(onRadioIRQ);
  restartReceive();
  if (state == RADIOLIB_ERR_NONE) { ++txPackets; ++radioQuality.txOK; radioQuality.txBytes+=size; lastTransmitOK = true; }
  else { radioError = state; ++radioQuality.txErrors; }
  return true;
}
void queueAck(const Header &h, uint8_t result) {
  ack.active = true; ack.transfer = h.transfer; ack.index = h.index; ack.result = result;
}
void resetIncoming() {
  incoming.stream.close(); incoming.active = false;
  LittleFS.remove("/incoming.part");
}
void receiveBegin(const Header &h, const uint8_t *data) {
  if (h.index != BEGIN_INDEX || h.length < 39) { queueAck(h, INVALID); return; }
  uint32_t size; memcpy(&size, data + 1, 4);
  uint8_t nameLen = data[37], authorLen = data[38];
  if (data[0] > 1 || !size || size > (data[0] ? MAX_FILE : MAX_TEXT) ||
      nameLen > 80 || authorLen > 32 || h.length != 39 + nameLen + authorLen) {
    queueAck(h, INVALID); return;
  }
  String id = messageId(h.session, h.transfer, h.from);
  Message *existing = findMessage(id);
  if (existing && !existing->outgoing && existing->status == "received") { queueAck(h, chat::OK); return; }
  if (incoming.active) {
    if (incoming.session == h.session && incoming.transfer == h.transfer) { queueAck(h, chat::OK); return; }
    queueAck(h, chat::BUSY); return;
  }
  if (!storageAvailable()) { queueAck(h, STORAGE); return; }
  incoming.message = Message();
  auto &m = incoming.message;
  m.id = id; m.file = data[0]; m.size = size; m.status = "received";
  for (size_t i = 0; i < nameLen; ++i) m.name += char(data[39 + i]);
  for (size_t i = 0; i < authorLen; ++i) m.author += char(data[39 + nameLen + i]);
  m.name = cleanLabel(m.name, 80); m.author = cleanLabel(m.author, 32);
  memcpy(incoming.hash, data + 5, 32);
  incoming.stream = LittleFS.open("/incoming.part", "w");
  if (!incoming.stream) { queueAck(h, STORAGE); return; }
  incoming.active = true; incoming.session = h.session; incoming.transfer = h.transfer;
  incoming.expected = 0; incoming.touched = millis();
  queueAck(h, chat::OK);
}
void receiveContent(const Header &h, const uint8_t *data) {
  String id = messageId(h.session, h.transfer, h.from);
  Message *existing = findMessage(id);
  if (existing && !existing->outgoing && existing->status == "received") {
    if ((h.type == END && h.index == END_INDEX && !h.length) ||
        (h.type == DATA && h.length == chunkSize(existing->size, h.index) && h.length))
      { ++radioQuality.duplicates; queueAck(h, chat::OK); }
    return;
  }
  if (!incoming.active || incoming.session != h.session || incoming.transfer != h.transfer) {
    queueAck(h, INVALID); return;
  }
  incoming.touched = millis();
  if (h.type == DATA) {
    if (!h.length || h.length != chunkSize(incoming.message.size, h.index) || h.index > incoming.expected) {
      queueAck(h, INVALID); return;
    }
    if (h.index < incoming.expected) ++radioQuality.duplicates;
    if (h.index == incoming.expected) {
      if (incoming.stream.write(data, h.length) != h.length) {
        resetIncoming(); queueAck(h, STORAGE); return;
      }
      ++incoming.expected;
    }
    queueAck(h, chat::OK); return;
  }
  if (h.index != END_INDEX || h.length || incoming.expected != chunks(incoming.message.size)) {
    queueAck(h, INVALID); return;
  }
  incoming.stream.close();
  uint8_t hash[32];
  if (!hashFile("/incoming.part", hash) || memcmp(hash, incoming.hash, 32) != 0) {
    resetIncoming(); queueAck(h, HASH); return;
  }
  Message m = incoming.message;
  if (!LittleFS.rename("/incoming.part", dataPath(m.id))) {
    resetIncoming(); queueAck(h, STORAGE); return;
  }
  incoming.active = false;
  if (m.file) m.mime = sniffMime(dataPath(m.id));
  else {
    File file = LittleFS.open(dataPath(m.id), "r");
    m.text = file.readString();
  }
  pruneHistory(); messages.push_back(m); saveHistory();
  queueAck(h, chat::OK);
  Serial.printf("RECEIVED %s %u bytes\n", m.id.c_str(), m.size);
}
void receiveAck(const Header &h, const uint8_t *data) {
  if (jobs.empty() || h.length != 1) return;
  auto &j = jobs.front();
  if (!j.waiting || h.transfer != j.transfer || h.index != j.index) return;
  if (data[0] == chat::BUSY) { j.deadline = millis() + 15000 + esp_random() % 3000; return; }
  if (data[0] != chat::OK) {
    if ((data[0] == INVALID || data[0] == HASH) && j.restarts++ < 2) {
      j.index = BEGIN_INDEX; j.waiting = false; j.attempts = 0; return;
    }
    failJob(data[0] == STORAGE ? "Druhá deska nemá místo v úložišti." : "Druhá deska odmítla přenos.");
    return;
  }
  if (j.attempts > 1) ++radioQuality.recovered;
  Message *m = findMessage(j.id);
  if (!m) { failJob("Chybí místní zpráva."); return; }
  if (j.index == END_INDEX) {
    m->status = "delivered";
    ++radioQuality.delivered;
    Serial.printf("DELIVERED %s\n", m->id.c_str());
    jobs.pop_front(); saveHistory(); return;
  }
  j.index = j.index == BEGIN_INDEX ? 0 : j.index + 1;
  if (j.index >= chunks(m->size)) j.index = END_INDEX;
  j.waiting = false; j.attempts = 0;
}
#include "radio_lab.h"
#include "radio_quality_runtime.h"

void receiveRadio() {
  if (!radioReady || !radioIRQ) return;
  radioIRQ = false;
  uint32_t irq=radio.getIrqFlags();
  if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) {
    if (irq & RADIOLIB_SX126X_IRQ_HEADER_ERR) radioQuality.receive(millis(),RX_HEADER);
    else if (irq & RADIOLIB_SX126X_IRQ_CRC_ERR) radioQuality.receive(millis(),RX_CRC);
    else radioQuality.receive(millis(),RX_IO);
    restartReceive(); return;
  }
  size_t size = radio.getPacketLength();
  uint8_t frame[255], payload[MAX_FRAME_PAYLOAD], nonce[12];
  int state = radio.readData(frame, sizeof(frame));
  float rssi = radio.getRSSI(), snr = loraProfile(activeProfile) ? radio.getSNR() : 0;
  restartReceive();
  if (state != RADIOLIB_ERR_NONE) {
    radioQuality.receive(millis(),state==RADIOLIB_ERR_CRC_MISMATCH?RX_CRC:RX_IO); return;
  }
  if (size < sizeof(Header) + TAG_SIZE || size > sizeof(frame)) { radioQuality.receive(millis(),RX_FORMAT); return; }
  Header h; memcpy(&h, frame, sizeof(h));
  if (!validHeader(h, size, NODE_ID)) { ++rejectedPackets; radioQuality.receive(millis(),RX_FORMAT); return; }
  memcpy(nonce, &h.session, 8); memcpy(nonce + 8, &h.counter, 4);
  if (mbedtls_gcm_auth_decrypt(&cipher, h.length, nonce, 12, frame, sizeof(h),
      frame + sizeof(h) + h.length, TAG_SIZE, frame + sizeof(h), payload) != 0) {
    ++rejectedPackets; radioQuality.receive(millis(),RX_AUTH); return;
  }
  if (h.type == HELLO) {
    if (h.length != 16) { radioQuality.receive(millis(),RX_FORMAT); return; }
    uint64_t challenge, echo;
    memcpy(&challenge, payload, 8); memcpy(&echo, payload + 8, 8);
    auto result = peerLink.hello(h.session, h.counter, challenge, echo, h.index,
      millis(), randomChallenge(), esp_random());
    if (result.accepted) {
      if (result.changed) {
        if (bench.active) { stopBenchmark(false); labMessage = "Test přerušil restart druhé desky."; }
        if (sim.active) { stopSimulation(false); labMessage = "RTK test přerušil restart druhé desky."; }
        plan.active = false;
        if (incoming.active) resetIncoming();
        if (!jobs.empty()) { jobs.front().index = BEGIN_INDEX; jobs.front().waiting = false; jobs.front().attempts = 0; }
        ack.active = false;
        Serial.println("PAIR authenticated");
      }
      peerSwitch = h.transfer == (SWITCH_CAPABILITY | activeProfile);
      peerSim = peerSwitch || h.transfer == (SIM_CAPABILITY | activeProfile);
      peerLab = peerSim || h.transfer == (LAB_CAPABILITY | activeProfile);
    } else { radioQuality.receive(millis(),RX_HANDSHAKE); return; }
  } else {
    if (!peerLink.packet(h.session, h.counter, millis())) { ++rejectedPackets; radioQuality.receive(millis(),RX_SESSION); return; }
    if (h.type >= LAB_REQUEST) receiveLab(h, payload);
    else if (labBusy() && h.type != ACK) { queueAck(h, chat::BUSY); }
    else if (h.type == ACK) receiveAck(h, payload);
    else if (h.type == BEGIN) receiveBegin(h, payload);
    else receiveContent(h, payload);
  }
  lastRSSI = rssi; lastSNR = snr; ++rxPackets;
  radioQuality.receive(millis(),RX_OK,size,rssi,snr,loraProfile(activeProfile));
  radioQuality.sequence(h.session,h.counter);
}
void sendHello() {
  uint64_t hello[] = {peerLink.challenge, peerLink.peerChallenge};
  uint16_t flags = peerLink.wantReply() ? HELLO_REQUEST_REPLY : 0;
  if (transmitPacket(HELLO, SWITCH_CAPABILITY | activeProfile, flags, reinterpret_cast<uint8_t *>(hello), sizeof(hello)))
    peerLink.transmitted(millis(), lastTransmitOK, esp_random(), packetAirtime(sizeof(Header) + sizeof(hello) + TAG_SIZE));
}
void tickRadio() {
  if (otaInProgress || rebootAt) return;
  receiveRadio();
  maintainLab();
  uint32_t now = millis();
  if (incoming.active && now - incoming.touched > RX_TIMEOUT) resetIncoming();
  if (!radioReady || !due(now, nextTx)) return;
  if (ack.active) {
    uint8_t result = ack.result;
    if (transmitPacket(ACK, ack.transfer, ack.index, &result, 1) && lastTransmitOK) ack.active = false;
    return;
  }
  // Authentication must also work while a test owns the data scheduler.
  if (peerLink.urgent(now)) { sendHello(); return; }
  if (tickLab()) return;
  bool chatReady = !jobs.empty() && peerSession && now - lastPeer < LINK_STALE_MS &&
    (!jobs.front().waiting || due(now, jobs.front().deadline));
  if (!chatReady) {
    if (peerLink.idleDue(now)) { peerLink.request(now); sendHello(); }
    return;
  }
  auto &j = jobs.front();
  if (j.waiting && !due(now, j.deadline)) return;
  if (j.attempts >= 8) { failJob("Doručení se nepotvrdilo. Zkontrolujte druhou desku a dosah."); return; }
  Message *m = findMessage(j.id);
  if (!m) { failJob("Chybí zpráva."); return; }
  uint8_t payload[CHUNK] = {}, type;
  size_t length = 0;
  if (j.index == BEGIN_INDEX) {
    type = BEGIN; payload[0] = m->file;
    memcpy(payload + 1, &m->size, 4);
    if (!hashFile(dataPath(m->id), payload + 5)) { failJob("Nelze číst soubor."); return; }
    payload[37] = m->name.length(); payload[38] = m->author.length();
    memcpy(payload + 39, m->name.c_str(), m->name.length());
    memcpy(payload + 39 + m->name.length(), m->author.c_str(), m->author.length());
    length = 39 + m->name.length() + m->author.length();
  } else if (j.index == END_INDEX) type = END;
  else {
    type = DATA; length = chunkSize(m->size, j.index);
    File file = LittleFS.open(dataPath(m->id), "r");
    if (!file || !file.seek(uint32_t(j.index) * CHUNK) || file.read(payload, length) != length) {
      failJob("Nelze číst soubor."); return;
    }
  }
  if (transmitPacket(type, j.transfer, j.index, payload, length)) {
    if (j.attempts++) { ++retransmits; ++radioQuality.retries; }
    j.waiting = true;
    j.deadline = millis() + ACK_TIMEOUT + esp_random() % 3000;
    m->status = "sending";
  }
}

void makeState(JsonDocument &doc, bool includeMessages = true) {
  doc["node"] = NODE_NAME; doc["ssid"] = String("LoRaChat-") + NODE_NAME;
  doc["version"] = FIRMWARE_VERSION; doc["ota"] = true;
  doc["token"] = csrfToken; doc["radioReady"] = radioReady; doc["radioError"] = radioError;
  doc["storageReady"] = filesystemReady;
  doc["paired"] = peerSession != 0; doc["online"] = peerSession && millis() - lastPeer < LINK_OFFLINE_MS;
  doc["peerAge"] = peerSession ? (millis() - lastPeer) / 1000 : -1;
  doc["rssi"] = lastRSSI;
  if (loraProfile(activeProfile)) doc["snr"] = lastSNR; else doc["snr"] = nullptr;
  appendLabState(doc);
  doc["txPackets"] = txPackets; doc["rxPackets"] = rxPackets;
  auto link = doc["link"].to<JsonObject>();
  link["searching"] = peerLink.searching; link["helloPackets"] = helloPackets;
  link["servicePackets"] = servicePackets; link["dataPackets"] = dataPackets;
  link["serviceAirtimeMs"] = serviceAirtimeUs / 1000; link["dataAirtimeMs"] = dataAirtimeUs / 1000;
  uint64_t totalAirtime = serviceAirtimeUs + dataAirtimeUs;
  link["servicePercent"] = totalAirtime ? 100.0 * serviceAirtimeUs / totalAirtime : 0;
  link["radioRecoveries"] = radioRecoveries;
  doc["retries"] = retransmits; doc["rejected"] = rejectedPackets;
  doc["queue"] = jobs.size(); doc["maxFile"] = MAX_FILE; doc["maxText"] = MAX_TEXT;
  doc["uptime"] = millis() / 1000; doc["freeHeap"] = ESP.getFreeHeap();
  doc["freeStorage"] = filesystemReady ? LittleFS.totalBytes() - LittleFS.usedBytes() : 0;
  doc["txWaitMs"] = due(millis(), nextTx) ? 0 : nextTx - millis();
  if (incoming.active) {
    auto r = doc["receiving"].to<JsonObject>();
    r["name"] = incoming.message.file ? incoming.message.name : "Textová zpráva";
    r["received"] = uint32_t(incoming.expected) * CHUNK < incoming.message.size ? uint32_t(incoming.expected) * CHUNK : incoming.message.size;
    r["size"] = incoming.message.size;
  }
  if (!jobs.empty()) {
    auto &j = jobs.front(); Message *m = findMessage(j.id);
    auto p = doc["transfer"].to<JsonObject>();
    p["id"] = j.id; p["attempt"] = j.attempts;
    p["sent"] = !m || j.index == BEGIN_INDEX ? 0 : j.index == END_INDEX ? m->size : uint32_t(j.index) * CHUNK;
    p["size"] = m ? m->size : 0;
  }
  if (includeMessages) {
    auto list = doc["messages"].to<JsonArray>();
    for (const auto &m : messages) encodeMessage(list.add<JsonObject>(), m);
  }
}
void jsonReply(int code, const String &error = "") {
  JsonDocument doc; doc["ok"] = error.isEmpty();
  if (!error.isEmpty()) doc["error"] = error;
  String body; serializeJson(doc, body);
  web.sendHeader("Cache-Control", "no-store");
  web.send(code, "application/json", body);
}
bool authorized() {
  if (web.header("X-Chat-Token") != csrfToken) { jsonReply(403, "Obnovte stránku a zkuste akci znovu."); return false; }
  return true;
}
void serveUI() {
  web.sendHeader("Content-Encoding", "gzip");
  web.sendHeader("Cache-Control", "no-cache");
  web.sendHeader("X-Content-Type-Options", "nosniff");
  web.sendHeader("Content-Security-Policy", "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
  web.send_P(200, "text/html; charset=utf-8", reinterpret_cast<const char *>(WEB_GZ), sizeof(WEB_GZ));
}
void handleUpload() {
  auto &u = web.upload();
  if (u.status == UPLOAD_FILE_START) {
    if (uploading) { uploadFile.close(); LittleFS.remove("/upload.part"); }
    uploadError = ""; uploadSize = 0; uploading = false; uploadFinished = false;
    if (web.header("X-Chat-Token") != csrfToken) { uploadError = "Neplatný přístupový token."; return; }
    if (otaInProgress || rebootAt) { uploadError = "Probíhá aktualizace firmwaru."; return; }
    if (labBusy()) { uploadError = "Nejdřív dokončete test nebo přepnutí rádia."; return; }
    if (!radioReady || !filesystemReady) { uploadError = "Rádio nebo úložiště není připraveno."; return; }
    if (jobs.size() >= QUEUE_LIMIT || !storageAvailable()) { uploadError = "Fronta nebo úložiště je plné."; return; }
    uploadName = cleanLabel(u.filename, 80);
    if (uploadName.isEmpty()) uploadName = "soubor.bin";
    uploadAuthor = cleanLabel(web.arg("author"), 32);
    if (uploadAuthor.isEmpty()) uploadAuthor = String("Stanice ") + NODE_NAME;
    uploadFile = LittleFS.open("/upload.part", "w");
    uploading = bool(uploadFile);
    if (!uploading) uploadError = "Nelze uložit soubor.";
  } else if (u.status == UPLOAD_FILE_WRITE && uploading) {
    uploadSize += u.currentSize;
    if (uploadSize > MAX_FILE) uploadError = "Soubor překročil 64 kB.";
    if (uploadError.isEmpty() && uploadFile.write(u.buf, u.currentSize) != u.currentSize) uploadError = "Úložiště je plné.";
  } else if (u.status == UPLOAD_FILE_END && uploading) {
    uploadFile.close(); uploading = false; uploadFinished = true;
    if (!uploadSize) uploadError = "Soubor je prázdný.";
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    uploadFile.close(); uploading = false; uploadFinished = false;
    LittleFS.remove("/upload.part"); uploadError = "Nahrávání bylo přerušeno.";
  }
  uploadTouched = millis();
  // WebServer streams multipart in this callback; keep radio ACKs responsive.
  tickRadio();
}
void failOTA(const String &error) {
  if (otaError.isEmpty()) otaError = error;
  if (Update.isRunning()) Update.abort();
  otaInProgress = false;
}
void handleOTA() {
  auto &u = web.upload();
  otaTouched = millis();
  if (u.status == UPLOAD_FILE_START) {
    if (++otaFileCount != 1) { failOTA("Vyberte jen jeden aktualizační soubor."); return; }
    otaError = ""; otaFinished = false; otaBytes = 0; otaHeaderBytes = 0;
    if (web.header("X-Chat-Token") != csrfToken) { failOTA("Neplatný přístupový token."); return; }
    if (!jobs.empty() || incoming.active || uploading || rebootAt || labBusy()) {
      failOTA("Před aktualizací dokončete nebo zrušte přenosy."); return;
    }
    otaInProgress = true;
  } else if (u.status == UPLOAD_FILE_WRITE && otaInProgress) {
    uint8_t *data = u.buf;
    size_t length = u.currentSize;
    if (otaHeaderBytes < sizeof(otaHeader)) {
      size_t n = min(length, sizeof(otaHeader) - otaHeaderBytes);
      memcpy(reinterpret_cast<uint8_t *>(&otaHeader) + otaHeaderBytes, data, n);
      otaHeaderBytes += n; data += n; length -= n;
      if (otaHeaderBytes < sizeof(otaHeader)) return;
      if (!validOtaHeader(otaHeader, NODE_ID)) {
        failOTA("Nesprávný soubor. Použijte LoRaChat-" + String(NODE_NAME) + ".ota."); return;
      }
      uint8_t signature[32];
      int result = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), PAIR_KEY,
          sizeof(PAIR_KEY), reinterpret_cast<uint8_t *>(&otaHeader), offsetof(OtaHeader, hmac), signature);
      if (result || !equalDigest(signature, otaHeader.hmac)) {
        failOTA("Soubor nepatří tomuto páru nebo má poškozený podpis."); return;
      }
      if (!Update.begin(otaHeader.size, U_FLASH)) { failOTA(Update.errorString()); return; }
      mbedtls_sha256_starts_ret(&otaHash, 0);
    }
    if (length) {
      if (otaBytes + length > otaHeader.size) { failOTA("Soubor je delší, než uvádí hlavička."); return; }
      if (Update.write(data, length) != length) { failOTA(Update.errorString()); return; }
      mbedtls_sha256_update_ret(&otaHash, data, length);
      otaBytes += length;
    }
  } else if (u.status == UPLOAD_FILE_END) {
    otaFinished = true;
    if (otaInProgress && (otaHeaderBytes != sizeof(otaHeader) || otaBytes != otaHeader.size))
      failOTA("Soubor nebyl nahrán celý.");
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    failOTA("Nahrávání bylo přerušeno. Původní firmware zůstává aktivní.");
    otaFinished = false; otaFileCount = 0;
  }
}
void finishOTA() {
  if (!authorized()) { failOTA("Neplatný přístupový token."); otaFileCount = 0; return; }
  if (otaError.isEmpty() && (!otaInProgress || !otaFinished || otaFileCount != 1))
    failOTA("Chybí úplný aktualizační soubor.");
  if (otaError.isEmpty()) {
    uint8_t digest[32]; mbedtls_sha256_finish_ret(&otaHash, digest);
    if (!equalDigest(digest, otaHeader.sha256)) failOTA("Kontrolní součet nesouhlasí. Aktualizace nebyla aktivována.");
    else if (!Update.end()) failOTA(Update.errorString());
  }
  bool success = otaError.isEmpty();
  jsonReply(success ? 200 : 400, otaError);
  otaInProgress = false; otaFinished = false; otaFileCount = 0;
  if (success) { Serial.println("OTA verified; restarting"); rebootAt = millis() + 2000; }
}
void setupWeb() {
  const char *headers[] = {"X-Chat-Token"}; web.collectHeaders(headers, 1);
  web.on("/", HTTP_GET, serveUI);
  web.on("/api/quality", HTTP_GET, [] {
    JsonDocument doc; makeQuality(doc); String result; serializeJson(doc,result);
    web.sendHeader("Cache-Control","no-store"); web.send(200,"application/json",result);
  });
  web.on("/api/state", HTTP_GET, [] {
    JsonDocument doc; makeState(doc); String result; serializeJson(doc, result);
    web.sendHeader("Cache-Control", "no-store"); web.send(200, "application/json", result);
  });
  web.on("/api/message", HTTP_POST, [] {
    if (!authorized()) return;
    if (web.arg("plain").length() > 10000) { jsonReply(413, "Zpráva je příliš dlouhá."); return; }
    JsonDocument doc;
    if (deserializeJson(doc, web.arg("plain"))) { jsonReply(400, "Neplatná zpráva."); return; }
    String error = queueText(doc["text"] | "", doc["author"] | "");
    jsonReply(error.isEmpty() ? 202 : 400, error);
  });
  web.on("/api/upload", HTTP_POST, [] {
    if (!authorized()) return;
    if (!uploadFinished && uploadError.isEmpty()) uploadError = "Chybí dokončený soubor.";
    if (uploadError.isEmpty()) {
      Message m; m.file = true; m.name = uploadName; m.author = uploadAuthor; m.size = uploadSize;
      uploadError = enqueue("/upload.part", m);
    }
    if (!uploadError.isEmpty()) LittleFS.remove("/upload.part");
    jsonReply(uploadError.isEmpty() ? 202 : 400, uploadError);
    uploadFinished = false;
  }, handleUpload);
  web.on("/api/update", HTTP_POST, finishOTA, handleOTA);
  web.on("/api/radio", HTTP_POST, [] {
    if (!authorized()) return;
    JsonDocument doc;
    if (web.arg("plain").length() > 128 || deserializeJson(doc, web.arg("plain")) || !doc["profile"].is<unsigned>() || doc["profile"].as<unsigned>() >= PROFILE_COUNT) {
      jsonReply(400, "Neplatný rádiový profil."); return;
    }
    if (!doc["confirmed"].is<bool>() || !doc["confirmed"].as<bool>()) {
      jsonReply(400, "Nejdřív potvrďte přepnutí obou desek."); return;
    }
    if (!doc["fromProfile"].is<unsigned>() || doc["fromProfile"].as<unsigned>() != activeProfile) {
      jsonReply(409, "Režim se mezitím změnil. Obnovte stránku a potvrďte znovu."); return;
    }
    String error = startLab(LabRequest{CHANGE_PROFILE, doc["profile"].as<uint8_t>(), 0, 0});
    jsonReply(error.isEmpty() ? 202 : 409, error);
  });
  web.on("/api/benchmark", HTTP_POST, [] {
    if (!authorized()) return;
    JsonDocument doc;
    if (web.arg("plain").length() > 128 || deserializeJson(doc, web.arg("plain")) ||
        !doc["direction"].is<unsigned>() || !doc["seconds"].is<unsigned>() ||
        doc["direction"].as<unsigned>() < 1 || doc["direction"].as<unsigned>() > 3 ||
        doc["seconds"].as<unsigned>() < 15 || doc["seconds"].as<unsigned>() > 600) {
      jsonReply(400, "Neplatný směr nebo délka testu."); return;
    }
    String error = startLab(LabRequest{START_BENCH, activeProfile, doc["direction"].as<uint8_t>(), doc["seconds"].as<uint16_t>()});
    jsonReply(error.isEmpty() ? 202 : 409, error);
  });
  web.on("/api/benchmark/stop", HTTP_POST, [] {
    if (!authorized()) return;
    if (plan.active) { jsonReply(409, "Počkejte na dokončení přípravy testu."); return; }
    stopBenchmark(true); labMessage = "Zastavuji test na obou deskách…"; jsonReply(200);
  });
  web.on("/api/rtk", HTTP_POST, [] {
    if (!authorized()) return;
    JsonDocument doc;
    if (web.arg("plain").length() > 256 || deserializeJson(doc, web.arg("plain")) ||
        !doc["direction"].is<unsigned>() || !doc["seconds"].is<unsigned>() ||
        !doc["preset"].is<unsigned>() || !doc["intervalMs"].is<unsigned>() || !doc["ttlMs"].is<unsigned>() ||
        doc["direction"].as<unsigned>() < 1 || doc["direction"].as<unsigned>() > 2 ||
        doc["seconds"].as<unsigned>() < 30 || doc["seconds"].as<unsigned>() > 3600 ||
        doc["preset"].as<unsigned>() > 2 || doc["intervalMs"].as<unsigned>() > 10000 || doc["ttlMs"].as<unsigned>() > 20000) {
      jsonReply(400, "Neplatné parametry RTK simulace."); return;
    }
    SimOptions o{doc["preset"].as<uint8_t>(), doc["intervalMs"].as<uint16_t>(), doc["ttlMs"].as<uint16_t>()};
    if (!validSimOptions(o)) { jsonReply(400, "Neplatný interval nebo stáří dávky."); return; }
    String error = startLab({START_SIM, activeProfile, doc["direction"].as<uint8_t>(), doc["seconds"].as<uint16_t>()}, o);
    jsonReply(error.isEmpty() ? 202 : 409, error);
  });
  web.on("/api/rtk/stop", HTTP_POST, [] {
    if (!authorized()) return;
    if (plan.active) { jsonReply(409, "Počkejte na dokončení přípravy testu."); return; }
    stopSimulation(true); jsonReply(200);
  });
  web.on("/api/radio/rescue", HTTP_POST, [] {
    if (!authorized()) return;
    if (otaInProgress || rebootAt) { jsonReply(409, "Probíhá aktualizace."); return; }
    rescueRadio(); jsonReply(200);
  });
  web.on("/api/file", HTTP_GET, [] {
    String id = web.arg("id");
    Message *m = validId(id) ? findMessage(id) : nullptr;
    if (!m || !m->file) { jsonReply(404, "Soubor nenalezen."); return; }
    File file = LittleFS.open(dataPath(id), "r");
    if (!file) { jsonReply(404, "Soubor chybí v úložišti."); return; }
    bool preview = web.arg("preview") == "1" && m->mime.startsWith("image/");
    web.sendHeader("X-Content-Type-Options", "nosniff");
    web.sendHeader("Content-Security-Policy", "sandbox; default-src 'none'");
    web.sendHeader("Content-Disposition", preview ? "inline" : "attachment; filename=\"download.bin\"");
    web.streamFile(file, preview ? m->mime : "application/octet-stream");
  });
  web.on("/api/pair", HTTP_POST, [] {
    if (!authorized()) return;
    peerLink.request(millis()); peerLink.helloAt = millis(); jsonReply(200);
  });
  web.on("/api/cancel", HTTP_POST, [] {
    if (!authorized()) return;
    String id = web.arg("id");
    for (auto it = jobs.begin(); it != jobs.end(); ++it) if (it->id == id) {
      Message *m = findMessage(id); if (m) { m->status = "cancelled"; m->error = "Odesílání zrušeno; doručení mohlo již proběhnout."; }
      jobs.erase(it); saveHistory(); jsonReply(200); return;
    }
    jsonReply(404, "Přenos už není ve frontě.");
  });
  web.on("/api/clear", HTTP_POST, [] {
    if (!authorized()) return;
    if (!jobs.empty() || incoming.active || uploading) { jsonReply(409, "Nejdřív dokončete nebo zrušte přenosy."); return; }
    for (auto &m : messages) LittleFS.remove(dataPath(m.id));
    messages.clear(); saveHistory(); jsonReply(200);
  });
  web.onNotFound([] {
    if (web.uri().startsWith("/api/")) jsonReply(404, "Neznámý požadavek.");
    else { web.sendHeader("Location", "http://192.168.4.1/"); web.send(302, "text/plain", "Open chat"); }
  });
  web.begin();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      serialLine.trim();
      if (serialLine == "STATUS") {
        JsonDocument doc; makeState(doc); doc.remove("token");
        serializeJson(doc, Serial); Serial.println();
      } else if (serialLine.startsWith("SEND ")) {
        String error = queueText(serialLine.substring(5), "USB test");
        Serial.println(error.isEmpty() ? "QUEUED" : "ERROR " + error);
      } else if (serialLine.startsWith("TESTFILE ")) {
        uint32_t size = serialLine.substring(9).toInt();
        if (size && size <= MAX_FILE && storageAvailable() && radioReady && !labBusy()) {
          File file = LittleFS.open("/test.tmp", "w");
          size_t written = 0;
          for (uint32_t i = 0; i < size && file; ++i) written += file.write(uint8_t(i * 31 + 7));
          file.close();
          Message m; m.file = true; m.name = "test-" + String(size) + ".bin";
          m.author = "USB test"; m.size = size;
          String error = written == size ? enqueue("/test.tmp", m) : "Test file write failed";
          Serial.println(error.isEmpty() ? "QUEUED" : "ERROR " + error);
        } else Serial.println("ERROR invalid test size, radio or storage");
      } else if (serialLine.startsWith("HASH ")) {
        String id = serialLine.substring(5); uint8_t digest[32];
        if (validId(id) && findMessage(id) && hashFile(dataPath(id), digest))
          Serial.println("HASH " + id + " " + digestHex(digest));
        else Serial.println("ERROR file not found");
      } else if (serialLine.startsWith("PROFILE ")) {
        unsigned profile = 0; char extra;
        bool valid = sscanf(serialLine.c_str(), "PROFILE %u %c", &profile, &extra) == 1 && profile < PROFILE_COUNT;
        String error = valid ? startLab(LabRequest{CHANGE_PROFILE, uint8_t(profile), 0, 0}) : "Invalid profile";
        Serial.println(error.isEmpty() ? "LAB queued" : "ERROR " + error);
      } else if (serialLine.startsWith("BENCH ")) {
        unsigned direction = 0, seconds = 0;
        bool valid = sscanf(serialLine.c_str(), "BENCH %u %u", &direction, &seconds) == 2 && direction >= 1 && direction <= 3 && seconds >= 15 && seconds <= 600;
        String error = valid ? startLab(LabRequest{START_BENCH, activeProfile, uint8_t(direction), uint16_t(seconds)}) : "Invalid test";
        Serial.println(error.isEmpty() ? "LAB queued" : "ERROR " + error);
      } else if (serialLine.startsWith("RTK ")) {
        unsigned direction, seconds, preset, interval, ttl; char extra;
        bool valid = sscanf(serialLine.c_str(), "RTK %u %u %u %u %u %c", &direction, &seconds, &preset, &interval, &ttl, &extra) == 5 &&
          direction >= 1 && direction <= 2 && seconds >= 30 && seconds <= 3600 && preset < 3 && interval <= 10000 && ttl <= 20000;
        String error = valid ? startLab({START_SIM, activeProfile, uint8_t(direction), uint16_t(seconds)},
          {uint8_t(preset), uint16_t(interval), uint16_t(ttl)}) : "Invalid RTK test";
        Serial.println(error.isEmpty() ? "LAB queued" : "ERROR " + error);
      } else if (serialLine == "RTKSTOP") { stopSimulation(true); Serial.println("LAB stopping"); }
      else if (serialLine == "BENCHSTOP") { stopBenchmark(true); Serial.println("LAB stopping"); }
      else if (serialLine == "RESCUE") { rescueRadio(); Serial.println("LAB default"); }
      else if (serialLine == "PAIR") { peerLink.request(millis()); peerLink.helloAt = millis(); Serial.println("PAIR requested"); }
      else if (serialLine == "SELFTEST") {
        uint8_t digest[32]; mbedtls_sha256_ret(reinterpret_cast<const uint8_t *>("abc"), 3, digest, 0);
        bool ok = digestHex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        uint8_t zeroKey[32] = {}, nonce[12] = {}, plain[16] = {}, encrypted[16], decrypted[16], tag[16];
        mbedtls_gcm_context test; mbedtls_gcm_init(&test);
        ok &= mbedtls_gcm_setkey(&test, MBEDTLS_CIPHER_ID_AES, zeroKey, 256) == 0;
        ok &= mbedtls_gcm_crypt_and_tag(&test, MBEDTLS_GCM_ENCRYPT, 16, nonce, 12, nullptr, 0, plain, encrypted, 16, tag) == 0;
        ok &= mbedtls_gcm_auth_decrypt(&test, 16, nonce, 12, nullptr, 0, tag, 16, encrypted, decrypted) == 0;
        ok &= memcmp(plain, decrypted, 16) == 0;
        tag[0] ^= 1;
        ok &= mbedtls_gcm_auth_decrypt(&test, 16, nonce, 12, nullptr, 0, tag, 16, encrypted, decrypted) != 0;
        mbedtls_gcm_free(&test);
        Serial.println(ok && chunks(MAX_FILE) == 365 ? "SELFTEST OK" : "SELFTEST FAILED");
      }
      serialLine = "";
    } else if (c != '\r') {
      if (serialLine.length() < 8192) serialLine += c;
      else serialLine = "";
    }
  }
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP((String("LoRaChat-") + NODE_NAME).c_str(), AP_PASSWORD, NODE_ID == 1 ? 1 : 6, false, 4);
  // Wi-Fi has started: ESP hardware RNG now has RF entropy.
  esp_fill_random(&sessionId, sizeof(sessionId));
  // Disjoint nonce spaces for the two senders, fresh random 63-bit boot session.
  sessionId = (sessionId & ~(uint64_t(1) << 63)) | (uint64_t(NODE_ID - 1) << 63);
  uint64_t token[2]; esp_fill_random(token, sizeof(token)); csrfToken = hex64(token[0]) + hex64(token[1]);
  mbedtls_gcm_init(&cipher);
  mbedtls_sha256_init(&otaHash);
  mbedtls_gcm_setkey(&cipher, MBEDTLS_CIPHER_ID_AES, PAIR_KEY, 256);
  filesystemReady = LittleFS.begin(true);
  if (filesystemReady) {
    loadHistory(); LittleFS.remove("/incoming.part"); LittleFS.remove("/upload.part"); LittleFS.remove("/text.tmp");
  }
  SPI.begin(7, 8, 9, 41);
  // Official Meshtastic XIAO S3 variant: B2B pins, RXEN=38, DIO3 TCXO 1.8V.
  radio.setRfSwitchPins(38, RADIOLIB_NC);
  Preferences settings;
  uint8_t savedProfile = 0;
  if (settings.begin("lorachat", true)) { savedProfile = settings.getUChar("profile", 0); settings.end(); }
  configureRadio(savedProfile < PROFILE_COUNT ? savedProfile : 0);
  dns.start(53, "*", WiFi.softAPIP());
  setupWeb();
  Serial.printf("LoRaChat %s ready; radio=%d storage=%d; http://192.168.4.1\n", NODE_NAME, radioError, filesystemReady);
}
void loop() {
  if (rebootAt && due(millis(), rebootAt)) ESP.restart();
  tickRadio();
  if (radioReady && !radioIRQ && !otaInProgress && !rebootAt && due(millis(),radioQuality.sampleAt))
    radioQuality.channel(millis(),radio.getRSSI(false));
  dns.processNextRequest();
  web.handleClient();
  handleSerial();
  if (uploading && millis() - uploadTouched > 30000) {
    uploadFile.close(); uploading = false; uploadFinished = false; LittleFS.remove("/upload.part");
  }
  if (otaInProgress && millis() - otaTouched > 30000) { failOTA("Vypršel čas pro aktualizaci."); otaFileCount = 0; }
  delay(1);
}
