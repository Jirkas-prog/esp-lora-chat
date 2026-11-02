// Local simulator for browser tests. It never connects to a physical board or radio.
const http = require('node:http'), fs = require('node:fs'), path = require('node:path');
const state = {
  version: '1.6.1', node: 'A', token: 'local-test-token', ssid: 'Example-Station',
  ota: true, online: true, paired: true, radioReady: true, storageReady: true,
  rxPackets: 1, txPackets: 0, rssi: -80, snr: 8, peerAge: 2, retries: 0,
  queue: 0, freeStorage: 1048576, uptime: 60, transfer: null, receiving: null,
  radio: { profile: 0, name: 'LoRa SF7', modulation: 'LoRa', busy: false,
    peerSwitchCapable: true, peerCapable: true, peerRtkCapable: true,
    profiles: [{ id: 0, name: 'LoRa SF7', hint: 'Výchozí kompromis dosahu a rychlosti.', rawKbps: 5.47 },
      { id: 6, name: 'GFSK 150', hint: 'Nejrychlejší GMSK profil; nejprve přepni zblízka.', rawKbps: 150 }] },
  messages: [{ id: 'example', outgoing: true, author: 'Zrušit', text: 'Připraveno', status: 'queued' }],
};
http.createServer((request, response) => {
  const url = new URL(request.url, 'http://localhost');
  if (url.pathname === '/') {
    response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    response.end(fs.readFileSync(path.join(__dirname, '../web/index.html')));
  } else if (url.pathname === '/api/state') {
    response.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    response.end(JSON.stringify(state));
  } else {
    response.writeHead(404, { 'Content-Type': 'application/json' });
    response.end(JSON.stringify({ error: 'Neznámý požadavek.' }));
  }
}).listen(18740, '127.0.0.1', () => console.log('Local LoRa Chat browser fixture ready.'));
