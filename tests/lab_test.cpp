// Run the production coordinator with fake radio/storage peripherals, no physical device.
#include "rtk_sim.h"
#include "radio_quality.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
using namespace chat;
using std::min; using std::max; using String = std::string;
#define NODE_ID 1
constexpr uint8_t PEER = 2;
constexpr int RADIOLIB_ERR_NONE = 0, RADIOLIB_SX126X_SYNC_WORD_PRIVATE = 0, RADIOLIB_SHAPING_0_5 = 0;
uint32_t clockMs = 1000, randomValue = 100;
uint32_t millis() { return clockMs; }
uint32_t esp_random() { return ++randomValue; }
uint64_t randomChallenge() { return ++randomValue; }
struct JsonObject {
  JsonObject operator[](const char *) { return {}; }
  template<class T> JsonObject &operator=(const T &) { return *this; }
  template<class T> T to() { return {}; }
  template<class T> T add() { return {}; }
  operator uint32_t() const { return 0; }
};
using JsonDocument = JsonObject; using JsonArray = JsonObject;
struct FakeRadio {
  int error = 0;
  template<class... T> int begin(T...) { return error; }
  template<class... T> int beginFSK(T...) { return error; }
  template<class... T> int setSyncWord(T...) { return 0; }
  int setDataShaping(int) { return 0; } int setWhitening(bool) { return 0; }
  int setDio2AsRfSwitch(bool) { return 0; } int setCRC(int) { return 0; }
  void clearDio1Action() {} void setDio1Action(void(*)()) {}
  int startReceive() { return error; }
  uint32_t getTimeOnAir(size_t) { return 100000; }
} radio;
RadioQuality radioQuality{};
int startRadioReceive() { return radio.startReceive(); }
void onRadioIRQ() {}
struct FakeSerial { template<class... T> void printf(T...) {} } Serial;
uint8_t storedProfile = 255;
struct Preferences {
  bool begin(const char *, bool) { return true; } void end() {}
  uint8_t getUChar(const char *, uint8_t fallback) { return storedProfile==255 ? fallback : storedProfile; }
  void putUChar(const char *, uint8_t value) { storedProfile=value; }
};
PeerLink peerLink;
const uint64_t &peerSession = peerLink.session; const uint32_t &lastPeer = peerLink.lastRx;
bool radioReady = true, radioIRQ = false, lastTransmitOK = true, uploading = false, otaInProgress = false;
uint32_t nextTx = 1000, txCounter = 600, radioRetryAt = 0, radioRecoveries = 0, rebootAt = 0;
uint8_t radioFailures = 0; int radioError = 0;
struct { bool active = false; } incoming, ack;
struct Job { bool waiting = false; uint16_t index = 0; uint8_t attempts = 0; };
std::deque<Job> jobs;
void resetIncoming() { incoming.active = false; }
bool labBusy(); uint32_t packetAirtime(size_t);
struct Sent { uint8_t type; uint16_t index; uint32_t id; std::vector<uint8_t> payload; };
std::vector<Sent> sent;
bool transmitPacket(uint8_t type, uint32_t id, uint16_t index, const uint8_t *data, size_t n) {
  if (!radioReady || !due(clockMs,nextTx)) return false;
  uint32_t air=packetAirtime(sizeof(Header)+n+TAG_SIZE);
  clockMs+=(air+999)/1000; nextTx=clockMs+offTime(air); ++txCounter;
  Sent frame{type,index,id,{}}; if(n)frame.payload.assign(data,data+n);
  sent.push_back(frame); lastTransmitOK=true; return true;
}
#include "../src/radio_lab.h"
void idlePair() {
  profileTrial=ProfileTrial(); plan=LabPlan(); labReply=LabReply(); bench=Benchmark(); resetSimulation(); sent.clear();
  peerLink.reset(clockMs,10,0,1); peerLink.session=22; peerLink.contact(clockMs);
  peerLab=peerSim=peerSwitch=true; radioReady=true; activeProfile=3; nextTx=clockMs;
}
void receive(uint8_t type, uint32_t id, uint16_t index, const void *data, size_t length) {
  Header h{}; h.type=type; h.transfer=id; h.index=index; h.length=uint8_t(length);
  receiveLab(h,static_cast<const uint8_t *>(data));
}
void advance(uint32_t ms) { auto end=clockMs+ms; while (!due(clockMs,end)) { maintainLab(); if (due(clockMs,nextTx)) tickLab(); ++clockMs; } }
// Swap the actual coordinator globals between two modeled boards. Crypto and
// collision recovery are tested separately in link_policy_test; here the radio
// adapter only delivers frames when the two production profiles match.
struct Board {
  LabPlan plan{}; LabReply reply{}; ProfileTrial trial{}; PeerLink link{};
  uint8_t profile=3, saved=3; uint32_t available=0, counter=0;
  void load() {
    ::plan=plan; labReply=reply; profileTrial=trial; peerLink=link;
    activeProfile=profile; storedProfile=saved; nextTx=available; txCounter=counter;
    peerLab=peerSim=peerSwitch=true; radioReady=true;
  }
  void save() {
    plan=::plan; reply=labReply; trial=profileTrial; link=peerLink;
    profile=activeProfile; saved=storedProfile; available=nextTx; counter=txCounter;
  }
};
void profilePair(unsigned scenario, uint8_t target, uint32_t start=1000) {
  clockMs=start; idlePair(); Board boards[2];
  for(unsigned i=0;i<2;++i) {
    boards[i].available=clockMs; boards[i].link.reset(clockMs,10+i,0,uint8_t(i+1));
    boards[i].link.session=22; boards[i].link.contact(clockMs);
  }
  // Scenarios: 0 healthy, 1 all COMMIT lost, 2 no new-mode traffic,
  // 3 only one direction in new mode, 4 simultaneous conflicting requests,
  // 5 first request/reply/commit/proof dropped, 6 peer reboot during trial.
  boards[0].load(); assert(startLab({CHANGE_PROFILE,target,0,0}).empty()); boards[0].save();
  if(scenario==4) {
    boards[1].load(); assert(startLab({CHANGE_PROFILE,0,0,0}).empty()); boards[1].save();
  }
  bool dropped[2][15]={}, rebooted=false; unsigned proofs=0;
  while(clockMs-start<62000) {
    for(unsigned i=0;i<2;++i) {
      if(boards[i].profile==boards[1-i].profile) {
        // Model completed fresh authentication after configureRadio.
        boards[i].link.session=22; boards[i].link.contact(clockMs);
      }
      boards[i].load(); maintainLab();
      sent.clear(); if(due(clockMs,nextTx))tickLab();
      auto frames=sent; boards[i].save();
      for(auto &f:frames) {
        if(f.type==PROFILE_PROOF)++proofs;
        bool lost=boards[i].profile!=boards[1-i].profile ||
          (scenario==1 && f.type==LAB_COMMIT) ||
          ((scenario==2 || (scenario==3 && i==1)) && boards[i].profile==target);
        if(scenario==5 && !dropped[i][f.type]) { dropped[i][f.type]=true; lost=true; }
        if(!lost) {
          boards[1-i].load(); receive(f.type,f.id,f.index,f.payload.data(),f.payload.size()); boards[1-i].save();
        }
      }
    }
    if(scenario==6 && !rebooted && boards[0].trial.active && boards[1].trial.active) {
      // Peer powers up with its old saved profile and a fresh session.
      boards[1].profile=boards[1].saved; boards[1].trial.active=false; boards[1].plan.active=false;
      boards[0].link.session=33; rebooted=true;
    }
    ++clockMs;
  }
  // Two simultaneous users may both get BUSY, or one request can proceed once
  // its counterpart has been refused. Both boards must finish on the same profile.
  bool succeeds=scenario==0 || scenario==5 || scenario==4;
  uint8_t expected=scenario==4 ? boards[0].profile : succeeds?target:3;
  if(scenario==4)assert(expected==0 || expected==target || expected==3);
  for(auto &b:boards) {
    if(b.profile!=expected || b.saved!=expected)
      fprintf(stderr,"scenario=%u target=%u start=%u now=%u profile=%u saved=%u trial=%d plan=%d proofs=%u\n",
              scenario,target,start,clockMs,b.profile,b.saved,b.trial.active,b.plan.active,proofs);
    assert(b.profile==expected && b.saved==expected);
    assert(!b.trial.active && !b.plan.active);
  }
  if(succeeds && expected!=3)assert(proofs>2 && proofs<=26);
  idlePair();
}
int main() {
  idlePair();
  assert(startLab({START_SIM,3,1,60},{1,0,20000}).empty()); tickLab();
  uint8_t ok=chat::OK; receive(LAB_REPLY,plan.id,0,&ok,1);
  clockMs=nextTx; tickLab(); assert(plan.committed);
  auto packets=sent.size(); receive(LAB_REPLY,plan.id,1,&ok,1);
  advance(5000); assert(sent.size()==packets); // No more COMMIT after acknowledgment.
  advance(11000); assert(sim.active && !plan.active && sim.counts.generated>0);
  idlePair(); assert(startLab({START_BENCH,3,2,60}).empty());
  receive(LAB_REPLY,plan.id,0,&ok,1); tickLab(); packets=sent.size();
  advance(3000); assert(sent.size()>packets); // Lost COMMIT acknowledgment is retried.
  advance(13000); assert(bench.active && !sim.id);
  advance(5000); packets=sent.size(); advance(5000); assert(sent.size()==packets); // No empty receiver reports.
  uint8_t data[MAX_FRAME_PAYLOAD]; BenchReport report{benchElapsed(),1,0,0,0,0,1}; memcpy(data,&report,sizeof(report));
  for(size_t i=0;i<BENCH_BYTES;++i) data[sizeof(report)+i]=testByte(bench.id,0,i);
  receive(BENCH_DATA,bench.id,0,data,sizeof(data)); tickLab();
  assert(sent.back().type==BENCH_REPORT);
  packets=sent.size(); advance(9000); assert(sent.size()==packets);
  stopBenchmark(false); advance(21000); assert(bench.finalReports.sent<=3);
  idlePair(); auto counter=txCounter; nextTx=clockMs+9999; auto debt=nextTx;
  assert(configureRadio(2)); assert(txCounter==counter && nextTx==debt);
  saveProfile(2); assert(storedProfile==2);
  peerLink.session=22; peerLink.contact(clockMs); auto challenge=peerLink.challenge;
  radioReady=false; radioError=-1; radioRetryAt=clockMs; maintainLab();
  assert(radioReady && radioRecoveries==1 && peerLink.session==22 && peerLink.challenge==challenge && nextTx==debt);
  // Failed driver initialization backs off without resetting crypto counters or airtime debt.
  radio.error=-1; radioReady=false; radioRetryAt=clockMs; maintainLab();
  assert(!radioReady && radioRetryAt==clockMs+1000); clockMs=radioRetryAt; maintainLab();
  assert(radioRetryAt==clockMs+2000 && txCounter==counter);
  radio.error=0; clockMs=radioRetryAt; maintainLab(); assert(radioReady);
  peerLink.request(clockMs); advance(21000); assert(activeProfile==0 && storedProfile==0);
  idlePair(); startSimulation({START_SIM,3,1,300},{1,0,20000},123); peerLink.request(clockMs);
  advance(70000); assert(activeProfile==3 && sim.active); // Active range test keeps its modulation.
  for(uint8_t p=0;p<PROFILE_COUNT;++p) {
    if(p==3)continue;
    profilePair(0,p); profilePair(5,p);
  }
  for(unsigned scenario=1;scenario<=6;++scenario)profilePair(scenario,6);
  profilePair(0,5,UINT32_MAX-20000);
  idlePair(); peerSwitch=false; assert(!startLab({CHANGE_PROFILE,6,0,0}).empty());
  ProfileTrial proof; proof.begin(100,12,3,6,22);
  assert(!proof.receive(13,{3,6,1},101) && !proof.receive(12,{3,5,1},101));
  assert(!proof.receive(12,{3,6,2},101));
  assert(proof.receive(12,{3,6,1},101)); proof.echoed=true;
  assert(proof.success(102,22) && !proof.success(12101,22) && !proof.success(102,33));
  puts("PASS: two production coordinators, every profile, lost handshake packets, unreachable/one-way trial, simultaneous requests, peer reboot, rollback/persistence, old firmware, timer wrap");
  puts("PASS: production coordinator, COMMIT acknowledgment/retry, RTK start, quiet benchmark, final report cap, profile storage, radio recovery/debt, fallback and active-test lock");
}
