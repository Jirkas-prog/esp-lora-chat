#include "radio_quality.h"
#include <cassert>
#include <cstdio>
using namespace chat;
int main(){
  RadioQuality q{}; q.setProfile(0,100);
  assert(q.initialized && !q.hasSignal && !q.hasChannel && q.generation==1);
  q.receive(200,RX_OK,64,-100,-4,true); q.receive(300,RX_OK,40,-80,6,true);
  auto &b=q.bin(500); assert(b.accepted==2 && b.bytes==104 && b.rssiSum/b.signalSamples==-90);
  assert(b.snrSum/b.snrSamples==1 && q.hasSnr && q.lastSignalAt==300);
  q.receive(500,RX_CRC);q.receive(500,RX_HEADER);q.receive(500,RX_FORMAT);q.receive(500,RX_AUTH);q.receive(500,RX_SESSION);
  q.receive(500,RX_HANDSHAKE);q.receive(500,RX_IO);
  assert(b.rejected==5 && q.counts[RX_HANDSHAKE]==1 && q.counts[RX_IO]==1);
  q.channel(600,-110);q.channel(800,-115);q.channel(900,0);
  assert(b.channelMin==-115 && b.channelSamples==2 && q.channelRssi==-115 && q.channelAt==800);
  q.sequence(123,5);q.sequence(123,8);assert(q.gaps==2);q.sequence(456,50);assert(q.gaps==2);
  q.setProfile(0,1000);assert(q.counts[RX_OK]==2 && q.generation==1);
  q.setProfile(3,1100);assert(!q.hasSnr && q.counts[RX_OK]==0 && q.gaps==0 && q.generation==2);
  q.receive(1200,RX_OK,60,-90,0,false);assert(!q.hasSnr && q.bin(1200).snrSamples==0);
  q.receive(1200,RX_OK,60,NAN,NAN,true);assert(q.counts[RX_OK]==2 && q.lastSignalAt==1200);
  q.setProfile(0,UINT32_MAX-60000);
  uint32_t start=q.since;
  for(uint32_t i=0;i<180;++i)q.receive(start+i*1000,RX_OK,100,-95,1,true);
  uint32_t count=0,bytes=0,now=start+179000;
  for(const auto &s:q.bins)if(q.recent(s,now)){++count;bytes+=s.bytes;}
  assert(count==120 && bytes==12000); // Continuous 120-second ring across millis rollover.
  for(const auto &s:q.bins)assert(!q.recent(s,now+120001));
  q.channel(now+200000,-100);assert(q.bin(now+200000).accepted==0);
  puts("PASS: passive radio metrics, independent error classes, no fake SNR, gaps/reboot, profile reset, 120-second ring and timer rollover");
}
