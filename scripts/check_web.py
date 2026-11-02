from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
html = (root / 'web/index.html').read_text(encoding='utf-8')
script = re.search(r'<script>(.*?)</script>', html, re.S).group(1)
output = root / 'test-output/web.js'
output.parent.mkdir(exist_ok=True)
output.write_text(script, encoding='utf-8')
subprocess.run(['node', '--check', str(output)], check=True)
ids = re.findall(r'\bid="([^"]+)"', html)
assert len(ids) == len(set(ids)), 'Duplicate DOM ID'
assert set(re.findall(r"\$\('([^']+)'\)", script)) <= set(ids), 'Unknown DOM ID'
assert not re.search(r'<(?:script|link)[^>]+(?:src|href)="https?://', html), 'External asset breaks offline UI'
print('PASS: JavaScript syntax, DOM references, no remote UI dependencies')
signal = script.split('function linkStatus(s){', 1)[1].split('function renderSignal(s){', 1)[0]
check = '''const assert=require('node:assert/strict');
const sample={node:'A',paired:true,radioReady:true,rxPackets:1,rssi:-80,snr:8,peerAge:2,online:true};
assert.equal(linkStatus(sample).level,4);
assert.equal(linkStatus({...sample,rssi:-110}).level,2);
assert.equal(linkStatus({...sample,snr:-6}).level,1);
assert.equal(linkStatus({...sample,peerAge:90}).kind,'stale');
assert.equal(linkStatus({...sample,peerAge:90}).level,0);
assert.equal(linkStatus({...sample,peerAge:120}).kind,'offline');
assert.equal(linkStatus({...sample,online:false}).level,0);
assert.equal(linkStatus({...sample,paired:false}).level,0);
assert.equal(linkStatus({...sample,radioReady:false}).level,0);
assert.equal(linkStatus({...sample,rssi:NaN}).level,0);
assert.equal(linkStatus(null).label,'Wi-Fi odpojena');
assert.equal(linkStatus({...sample,radio:{modulation:'GFSK'},snr:null,rssi:-70}).level,4);
assert.equal(linkStatus({...sample,radio:{modulation:'GFSK'},snr:null,rssi:-96}).level,1);
assert.match(linkStatus({...sample,radio:{modulation:'GFSK'},snr:null}).detail,/GFSK/);
assert.equal(linkStatus({...sample,snr:null}).level,0);
console.log('PASS: LoRa strength, SNR limit, stale data, offline and unknown states');
'''
signal_test = root / 'test-output/signal-check.cjs'
signal_test.write_text('const tr=s=>s;\nfunction linkStatus(s){' + signal + check, encoding='utf-8')
subprocess.run(['node', str(signal_test)], check=True)
rtk_view = script.split('function rtkView(t){', 1)[1].split('function renderRtk(s,blocked){', 1)[0]
rtk_check = '''const assert=require('node:assert/strict');
assert.equal(rtkView(null).fresh,false);
const rx={lastAgeMs:300,rate:500,complete:10,active:true};
const tx={sent:12,active:true};
const receiver={local:rx,peer:tx,active:true,localSender:false,peerAgeMs:30000};
assert.equal(rtkView(receiver).fresh,true);
assert.equal(rtkView(receiver).age,300);
assert.equal(rtkView(receiver).lost,null);
const sender={...receiver,local:tx,peer:rx,localSender:true,peerAgeMs:2000};
assert.equal(rtkView(sender).age,2300);
assert.equal(rtkView({...sender,peerAgeMs:16000}).fresh,false);
assert.equal(rtkView({...sender,reportIntervalMs:20000,peerAgeMs:30000}).fresh,true);
assert.equal(rtkView({...sender,reportIntervalMs:20000,peerAgeMs:42000}).fresh,false);
assert.equal(rtkView({...sender,peer:undefined}).fresh,false);
assert.equal(rtkView({...receiver,local:{...rx,lastAgeMs:-1}}).age,-1);
const final={...receiver,active:false,local:{...rx,active:false},peer:{...tx,active:false}};
assert.equal(rtkView(final).lost,2);
assert.equal(rtkView({...final,peer:undefined}).lost,null);
console.log('PASS: RTK UI local/remote freshness, unknown data, frozen final results, whole-epoch losses');
'''
rtk_test = root / 'test-output/rtk-view-check.cjs'
rtk_test.write_text('function rtkView(t){' + rtk_view + rtk_check, encoding='utf-8')
subprocess.run(['node', str(rtk_test)], check=True)
switch_fn = script.split('function profileChangeRequest(s,profile,ask){', 1)[1].split("$('applyProfile').onclick", 1)[0]
switch_check = '''
const assert=require('node:assert/strict');
const radio={profile:0,name:'LoRa SF7',peerSwitchCapable:true,busy:false,
             profiles:[{id:0,name:'LoRa SF7'},{id:6,name:'GFSK 150'}]};
const s={online:true,radio};
let asked=0;
const accept=text=>{++asked; assert.match(text,/OBĚ desky/);assert.match(text,/GFSK 150/);return true;};
assert.deepEqual(profileChangeRequest(s,6,accept),{profile:6,fromProfile:0,confirmed:true});
assert.equal(asked,1);
assert.equal(profileChangeRequest(s,6,()=>false),null);
const forbidden=()=>assert.fail('Confirmation must not open for an invalid operation');
for(const state of [null,{...s,online:false},{...s,radio:{...radio,busy:true}},
                    {...s,radio:{...radio,peerSwitchCapable:false}}])
  assert.equal(profileChangeRequest(state,6,forbidden),null);
for(const target of [0,8,-1,NaN])assert.equal(profileChangeRequest(s,target,forbidden),null);
console.log('PASS: explicit profile confirmation, cancel, source profile, busy/offline/legacy guards');
'''
switch_test = root / 'test-output/profile-ui-check.cjs'
switch_test.write_text('const tr=s=>s;\nfunction profileChangeRequest(s,profile,ask){' + switch_fn + switch_check, encoding='utf-8')
subprocess.run(['node', str(switch_test)], check=True)
quality_fn = script.split('function qualityView(q){',1)[1].split('function qualitySvg(',1)[0]
quality_check = '''
const assert=require('node:assert/strict');
assert.equal(qualityView(null).fresh,false);
const q={radioReady:true,lora:true,signalAgeMs:0,rssi:-100,snr:-8,window:{rejectedPercent:0},
 samples:[[1000,1,0,50,-100,-8,-115],[0,0,1,0,null,null,-116],[121000,1,0,50,-90,1,-115]]};
assert.equal(qualityView(q).snr,-8);
assert.equal(qualityView(q).rejected,0);
assert.equal(qualityView(q).samples.length,2);
assert.equal(qualityView({...q,lora:false}).snr,null);
assert.equal(qualityView({...q,signalAgeMs:90000}).rssi,null);
assert.equal(qualityView({...q,radioReady:false}).rssi,null);
assert.equal(qualityView({...q,rssi:NaN}).rssi,null);
assert.equal(qualityView({...q,window:{rejectedPercent:null}}).rejected,null);
assert.equal(qualityView({...q,signalAgeMs:-1}).fresh,false);
console.log('PASS: quality UI negative SNR, no invented GFSK/SNR, stale/missing/invalid values and sample window');
'''
quality_test=root/'test-output/quality-ui-check.cjs'
quality_test.write_text('function qualityView(q){'+quality_fn+quality_check,encoding='utf-8')
subprocess.run(['node',str(quality_test)],check=True)
