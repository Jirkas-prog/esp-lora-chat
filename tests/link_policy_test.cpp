#include "link_policy.h"
#include <cassert>
#include <cstdio>
#include <vector>
using namespace chat;

uint32_t randomState = 17;
uint32_t random32() { randomState ^= randomState << 13; randomState ^= randomState >> 17; randomState ^= randomState << 5; return randomState; }
uint64_t nonce = 100;
struct Node { PeerLink link; uint64_t sid; uint32_t serial = 0, nextTx = 0, txUntil = 0, sent = 0; };
struct Frame { unsigned from; uint64_t sid, challenge, echo; uint32_t serial, ends; uint16_t flags; bool lost; };
struct Pair {
  Node nodes[2]; std::vector<Frame> air; uint32_t now = 1000, airtime = 10; unsigned loss = 0;
  Pair(uint32_t start = 1000, uint32_t duration = 10) : now(start), airtime(duration) { boot(0); boot(1); }
  void boot(unsigned i) {
    auto &n = nodes[i]; n = {}; n.sid = ++nonce; n.nextTx = n.txUntil = now;
    n.link.reset(now, ++nonce, random32(), uint8_t(i + 1), airtime * 1000);
  }
  bool paired() { return nodes[0].link.session == nodes[1].sid && nodes[1].link.session == nodes[0].sid &&
    !nodes[0].link.replyPending && !nodes[1].link.replyPending && !nodes[0].link.searching && !nodes[1].link.searching; }
  void step() {
    for (size_t i = 0; i < air.size();) {
      auto f = air[i];
      if (!due(now, f.ends)) { ++i; continue; }
      air.erase(air.begin() + i);
      if (!f.lost) nodes[1-f.from].link.hello(f.sid, f.serial, f.challenge, f.echo, f.flags, now, ++nonce, random32());
    }
    for (unsigned i = 0; i < 2; ++i) {
      auto &n = nodes[i];
      if (!due(now, n.nextTx)) continue;
      if (n.link.idleDue(now)) n.link.request(now);
      if (!n.link.urgent(now)) continue;
      bool collision = !due(now, nodes[1-i].txUntil);
      if (collision) for (auto &f : air) f.lost = true;
      air.push_back({i,n.sid,n.link.challenge,n.link.peerChallenge,++n.serial,now+airtime,
        uint16_t(n.link.wantReply() ? HELLO_REQUEST_REPLY : 0),collision || random32()%100 < loss});
      n.txUntil = now + airtime; n.nextTx = n.txUntil + offTime(airtime * 1000); ++n.sent;
      n.link.transmitted(n.txUntil, true, random32(), airtime * 1000);
    }
    ++now;
  }
  void run(uint32_t ms) { uint32_t end = now + ms; while (!due(now,end)) step(); }
  uint32_t connect(uint32_t limit) { uint32_t start = now; while (!paired() && now-start < limit) step(); assert(paired()); return now-start; }
};
int main() {
  for (uint32_t airtime : {10U, 110U, 370U}) {
    Pair p(1000,airtime); auto first = p.connect(60000);
    // Packet reception refreshes liveness without any heartbeat during continuous data.
    auto sent = p.nodes[0].sent + p.nodes[1].sent;
    for (int n=0;n<600;++n) {
      for (unsigned i=0;i<2;++i) assert(p.nodes[1-i].link.packet(p.nodes[i].sid,++p.nodes[i].serial,p.now));
      p.run(1000);
    }
    assert(sent == p.nodes[0].sent + p.nodes[1].sent);
    // A power cycle is authenticated while the other end retains the old session.
    auto old = p.nodes[1]; p.boot(1); auto reboot = p.connect(60000);
    auto &a = p.nodes[0]; auto last = a.link.lastRx;
    assert(!a.link.packet(a.link.session,a.link.counter,p.now)); assert(a.link.lastRx == last);
    assert(!a.link.hello(old.sid,old.serial+1,old.link.challenge,a.link.challenge,0,p.now,++nonce,0).accepted);
    assert(a.link.session == p.nodes[1].sid); // Replayed boot cannot replace a live session.
    p.connect(60000);
    printf("MODEL airtime=%u ms: initial=%u ms, peer reboot=%u ms; 10 min data, zero HELLO\n",airtime,first,reboot);
  }
  for (uint32_t air : {10U,110U,370U}) for (unsigned seed=1;seed<=50;++seed) {
    randomState = seed; Pair p(1000,air); p.loss=35; p.connect(180000);
    p.loss=100; p.nodes[0].link.request(p.now); auto sent=p.nodes[0].sent; p.run(120000);
    assert(p.nodes[0].sent-sent <= 20); // Bounded retry rate during a long outage.
    p.loss=0; p.connect(60000);
  }
  Pair wrap(UINT32_MAX-500); wrap.connect(60000); wrap.run(180000); assert(wrap.paired());
  Pair idle; idle.connect(60000); auto before=idle.nodes[0].sent+idle.nodes[1].sent;
  idle.run(180000); auto keepalives=idle.nodes[0].sent+idle.nodes[1].sent-before;
  assert(keepalives>=4 && keepalives<=6);
  printf("MODEL idle pair: %u HELLO / 180 s\n",keepalives);
  // Final results normally need three frames total, capped at three per node when lost.
  FinalReports a,b; assert(a.needed()); a.transmitted(); b.received(0);
  b.transmitted(); a.received(REPORT_FINAL_SEEN); assert(a.needed());
  a.transmitted(); b.received(REPORT_FINAL_SEEN); assert(!a.needed() && !b.needed());
  FinalReports lost; for(int n=0;n<3;++n) { assert(lost.needed()); lost.transmitted(); } assert(!lost.needed());
  assert(reportInterval(10000)==10000 && reportInterval(500000)==20000);
  assert(feedbackFits(UINT32_MAX-50,200,100000)); assert(!feedbackFits(100,210,100000));
  puts("PASS: pairing/reboot, packet loss/collisions, replay protection, bounded retries, silent data, idle liveness, final reports, timer rollover");
}
