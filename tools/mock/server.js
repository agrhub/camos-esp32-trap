#!/usr/bin/env node
// SPDX-License-Identifier: AGPL-3.0-only
/*
 * Mock camera for working on the setup portal and the web console without a board (and for the screenshots in
 * docs/img). Serves firmware/main/portal.html at /portal and firmware/main/console.html at /, with fake data only:
 * serial ESP000000000001, verify code ABC123, made-up Wi-Fi names, documentation IP addresses. The live picture and
 * the SD photos are drawn illustrations, not camera pictures.
 *
 *   node tools/mock/server.js [port]          (default 8099), then http://localhost:8099/
 *
 * Query options for screenshots: ?lang=vi (Vietnamese), ?gate=1 (activation form), ?unclaimed=1 (claim QR),
 * ?open=adv (portal: open "Advanced settings"); /phone?h=844&src=<url-encoded path> shows a page 390 px wide.
 */
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const url = require('url');

const ROOT = path.join(__dirname, '..', '..', 'firmware', 'main');
const PORT = +process.argv[2] || 8099;
const SERIAL = 'ESP000000000001', CODE = 'ABC123', MODEL = 'AIC-FREENOVE-S3CAM';
let flags = { gate: false, unclaimed: false };

const START = Date.now(), T0 = Date.UTC(2026, 9, 17, 3, 20, 0); /* a fixed morning: 10:20 in Asia/Ho_Chi_Minh */
const utcNow = () => new Date(T0 + Date.now() - START);
const now = () => new Date(utcNow().getTime() + 7 * 3600e3);
const local = (d = now()) => d.toISOString().replace('T', ' ').slice(0, 19);

/* ------------------------------------------------------------------ pictures (illustrations) */
function rnd(seed) { let s = seed; return () => (s = (s * 1103515245 + 12345) % 2147483648) / 2147483648; }
function trapSvg(w, h, seed, label) {
  const r = rnd(seed);
  let flies = '';
  for (let i = 0; i < 14; i++) {
    const x = 0.22 * w + r() * 0.56 * w, y = 0.2 * h + r() * 0.6 * h, a = r() * 180, s = w / 640;
    flies += `<g transform="translate(${x.toFixed(1)} ${y.toFixed(1)}) rotate(${a.toFixed(0)}) scale(${s.toFixed(2)})"><ellipse rx="7" ry="3.2" fill="#3a2a12"/><ellipse cx="-2" cy="-4" rx="6" ry="2.6" fill="#c9c2a8" opacity=".75"/><ellipse cx="-2" cy="4" rx="6" ry="2.6" fill="#c9c2a8" opacity=".75"/><circle cx="7" r="2.4" fill="#8a2a12"/></g>`;
  }
  let grid = '';
  for (let i = 1; i < 10; i++) grid += `<line x1="${0.2 * w + i * 0.06 * w}" y1="${0.16 * h}" x2="${0.2 * w + i * 0.06 * w}" y2="${0.84 * h}" stroke="#d9c400" stroke-width="1"/>`;
  for (let i = 1; i < 8; i++) grid += `<line x1="${0.2 * w}" y1="${0.16 * h + i * 0.085 * h}" x2="${0.8 * w}" y2="${0.16 * h + i * 0.085 * h}" stroke="#d9c400" stroke-width="1"/>`;
  return `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">
<defs><linearGradient id="bg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#4f6b3a"/><stop offset="1" stop-color="#2d4123"/></linearGradient></defs>
<rect width="${w}" height="${h}" fill="url(#bg)"/>
<circle cx="${0.1 * w}" cy="${0.2 * h}" r="${0.12 * w}" fill="#5d7d44" opacity=".6"/><circle cx="${0.92 * w}" cy="${0.75 * h}" r="${0.15 * w}" fill="#3c5530" opacity=".7"/>
<rect x="${0.2 * w}" y="${0.16 * h}" width="${0.6 * w}" height="${0.68 * h}" rx="4" fill="#f4dc12"/>${grid}${flies}
<rect x="${0.47 * w}" y="${0.05 * h}" width="${0.06 * w}" height="${0.11 * h}" fill="#ddd"/>
<text x="${w - 8}" y="${h - 10}" font-family="Segoe UI,Arial" font-size="${Math.round(h / 34)}" fill="#fff" fill-opacity=".85" text-anchor="end">${label}</text></svg>`;
}
function qrSvg(text) {
  /* a QR-like pattern for the mock (not a readable code) */
  const n = 29, r = rnd(text.length * 7919);
  let cells = '';
  const finder = (x, y) => (x < 7 && y < 7) || (x >= n - 7 && y < 7) || (x < 7 && y >= n - 7);
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) {
    let on;
    if (finder(x, y)) { const fx = x % (n - 7) === x ? x : x - (n - 7), fy = y % (n - 7) === y ? y : y - (n - 7); const d = Math.max(Math.abs(fx - 3), Math.abs(fy - 3)); on = d !== 2 && d !== 4; } else on = r() < 0.5;
    if (on) cells += `<rect x="${x + 2}" y="${y + 2}" width="1" height="1"/>`;
  }
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${n + 4} ${n + 4}" shape-rendering="crispEdges"><rect width="100%" height="100%" fill="#fff"/><g fill="#000">${cells}</g></svg>`;
}

/* ------------------------------------------------------------------ fake device */
const config = () => ({
  serial: SERIAL, model: MODEL, fw: '0.4.0', ssid: 'Orchard-AP', password: true, apn: '', simPin: false, apiBase: 'https://flybot.agrhub.com',
  token: !flags.unclaimed, provisionToken: false, iotHost: 'example-ats.iot.ap-southeast-2.amazonaws.com', thingName: 'cam-' + SERIAL, kvsChannel: 'cam-' + SERIAL,
  kvsRegion: 'ap-southeast-2', roleAlias: '', credHost: '', claimUrl: `https://flybot.agrhub.com/claim?sn=${SERIAL}`, timezone: 'Asia/Ho_Chi_Minh', apPassword: false,
  verifyCode: CODE, uiLang: 'en', motionRoi: '20,15,60,70', staticIp: '', staticMask: '255.255.255.0', staticGateway: '', staticDns: '',
  intervalMin: 30, powerMode: 'auto', cellular: true, gps: true, ledEnabled: true, batteryMonitor: true, claimed: !flags.unclaimed, sdSave: true, schedFrom: 330, schedTo: 1110,
  motion: true, motionSens: 60, motionCooldown: 60, motionUpload: false, motionSd: true, ipMode: 0,
  defaults: ['apn', 'apiBase', 'iotHost', 'kvsRegion', 'timezone', 'powerMode', 'cellular', 'gps', 'ledEnabled', 'sdSave', 'motionUpload', 'motionSd', 'ipMode'],
  defaultsFile: 'defaults.example.json', cert: !flags.unclaimed, key: true, paired: !flags.unclaimed, registered: !flags.unclaimed, hasModem: true, hasGps: true, hasLive: false,
  apSsid: 'CAM-0001', wifiError: '', regError: '', labelQr: `CAMOS:1;SN:${SERIAL};VC:${CODE};M:${MODEL};;`, timezoneAuto: 'Asia/Ho_Chi_Minh', timezoneSource: 'farm',
});
const status = () => ({
  model: MODEL, serial: SERIAL, verifyCode: CODE, fw: '0.4.0', built: 'Oct 17 2026 09:00:00', uptimeS: 3 * 86400 + 5 * 3600 + 17 * 60, heapFreeKB: 124, psramFreeKB: 7931,
  time: { synced: true, local: local(), utc: utcNow().toISOString().slice(0, 19) + 'Z', timezone: 'Asia/Ho_Chi_Minh' },
  net: { type: 'wifi', connected: true, ip: '192.168.1.50', rssi: -58, ssid: 'Orchard-AP', channel: 6, mask: '255.255.255.0', gateway: '192.168.1.1', mac: '02:00:00:00:00:01', apn: '' },
  cloud: { connected: !flags.unclaimed, host: 'example-ats.iot.ap-southeast-2.amazonaws.com', port: 8883, certificate: true, paired: !flags.unclaimed, thing: 'cam-' + SERIAL, apiBase: 'https://flybot.agrhub.com', kvsChannel: 'cam-' + SERIAL },
  image: { brightness: 0, contrast: 50, saturation: 50, jpegQuality: 85, shadowVersion: 12, w: 2560, h: 1920, sensor: 'OV5640' },
  battery: { voltage: 4.02, percent: 86, mv: 4020, pct: 86, charging: true, solarMv: 5650 }, power: { mode: 'auto', effective: 'powered', highMv: 4050, lowMv: 3850 },
  location: { lat: 10.9712, lng: 106.6503, fixAt: '2026-10-17T00:12:40Z' }, timezone: 'Asia/Ho_Chi_Minh', tzSource: 'farm', clockSource: 'sntp', publicIp: '203.0.113.7',
  claimed: !flags.unclaimed, intervalMin: 30, nextCaptureAt: '2026-10-17T03:30:00Z',
  led: { state: 'online (green blip)', enabled: true }, stream: { active: true, w: 640, h: 480, quality: 12, fps: 11.1, avgBytes: 17480, frames: 1234 },
  motion: { enabled: true, running: true, active: false, count: 3, level: 4, threshold: 13, lastAt: '2026-10-17T02:41:09Z' },
  sd: { present: true, totalMB: 29820, freeMB: 29102, save: true }, features: { modem: true, gps: true, live: false, sd: true },
});
const image = () => ({ video: { image: { brightness: 0, contrast: 50, saturation: 55, sharpness: 50, orientation: 0, whiteBalance: 'auto', effect: 'none', aeLevel: 0, exposureAuto: true, exposure: 300, gainAuto: true, gain: 0, gainCeiling: 2, denoise: 2 } },
  capture: { resolution: 'max', jpegQuality: 85, intervalMin: 30, w: 2560, h: 1920 }, sensor: 'OV5640', shadowVersion: 12 });
const days = ['20261017', '20261016', '20261015'];
const files = (day) => { const out = []; for (let m = 5 * 60 + 30; m <= 18 * 60 + 30; m += 30) { const hh = String(Math.floor(m / 60)).padStart(2, '0'), mm = String(m % 60).padStart(2, '0'); out.push({ name: `${hh}${mm}00S.JPG`, bytes: 240000 + (m * 37) % 90000, trigger: 'S' }); }
  if (day === days[0]) out.push({ name: '094109M.JPG', bytes: 31200, trigger: 'M' }, { name: '101522M.JPG', bytes: 30480, trigger: 'M' }, { name: '113005R.JPG', bytes: 251000, trigger: 'R' });
  return day === days[0] ? out.filter((f) => f.name < '120000') : out; };
const events = () => [
  ['10:00:00', 'capture', 'schedule: uploaded 01JABCDEF0123456789XYZ'], ['09:41:09', 'motion', '41 per mille changed, on SD'], ['09:30:00', 'capture', 'schedule: uploaded 01JABCDEF0123456789XYW'],
  ['09:12:30', 'image', 'image settings saved in the console'], ['09:00:00', 'capture', 'schedule: uploaded 01JABCDEF0123456789XYV'], ['08:30:00', 'capture', 'schedule: uploaded 01JABCDEF0123456789XYT'],
  ['07:02:11', 'sd', 'card mounted'], ['07:02:10', 'boot', 'power-on'],
].map(([t, type, text]) => ({ time: '2026-10-17 ' + t, uptimeS: 0, type, text }));
const schedule = () => ({ source: 'local', claimed: true, clockSynced: true, items: [{ id: 'interval', everyMin: 30, from: '05:30', to: '18:30' }],
  next: ['10:30', '11:00', '11:30', '12:00', '12:30', '13:00', '13:30', '14:00'].map((t) => ({ at: '2026-10-17 ' + t, id: 'interval' })), intervalMin: 30, schedFrom: 330, schedTo: 1110, powerEffective: 'powered' });
const scan = [['Orchard-AP', -52, 6, true], ['FarmHouse', -61, 1, true], ['Orchard-AP-5G', -67, 36, true], ['Pump-Station', -74, 11, true], ['Guest', -79, 6, false], ['Neighbour', -86, 3, true]]
  .map(([ssid, rssi, channel, secure]) => ({ ssid, rssi, channel, secure }));
const logs = `I (1023) camos: camos-esp32-trap ${MODEL} fw 0.4.0 serial ${SERIAL}
I (1031) camos: flash 16 MB, PSRAM 8192 KB
I (1164) sd: card SD32G, 29820 MB, 29102 MB free
I (5198) web: web console at http://192.168.1.50/ (admin password set)
I (8948) tz: clock set from sntp
I (9811) proto: MQTT connected to example-ats.iot.ap-southeast-2.amazonaws.com
I (9930) shadow: applied version 12: 0 schedule(s), interval 30, tz Asia/Ho_Chi_Minh
I (61022) sched: JPEG 2560x1920, 251340 bytes
I (61190) sd: saved 20261017/100000S.JPG (251340 bytes)
I (63871) flybot: snapshot uploaded
I (64012) stream: viewer started: 640x480 q12
I (74044) stream: 640x480 q12: 11.1 fps, 17480 bytes per frame
`;

/* ------------------------------------------------------------------ server */
const send = (res, code, type, body) => { res.writeHead(code, { 'Content-Type': type, 'Cache-Control': 'no-store' }); res.end(body); };
const json = (res, o, code = 200) => send(res, code, 'application/json', JSON.stringify(o));
function page(res, file, q) {
  let html = fs.readFileSync(path.join(ROOT, file), 'utf8');
  const lang = q.lang === 'vi' ? 'vi' : 'en';
  const pre = `<script>try{localStorage.clear();localStorage.setItem('camosLang','${lang}')}catch(e){}</script>`;
  const post = q.open === 'adv' ? `<script>setTimeout(()=>{const d=document.getElementById('adv');if(d){d.open=true;d.scrollIntoView();}},1200)</script>` : '';
  html = html.replace('<head>', '<head>' + pre).replace('</body>', post + '</body>');
  send(res, 200, 'text/html; charset=utf-8', html);
}
http.createServer((req, res) => {
  const u = url.parse(req.url, true), p = u.pathname, q = u.query;
  if (req.method === 'GET' && (p === '/' || p === '/index.html' || p === '/portal')) {
    flags = { gate: q.gate === '1', unclaimed: q.unclaimed === '1' };
    return page(res, p === '/portal' ? 'portal.html' : 'console.html', q);
  }
  if (p === '/phone') { /* phone-width frame for screenshots: /phone?h=844&src=/portal%3Flang%3Den */
    return send(res, 200, 'text/html', `<!doctype html><body style="margin:0;background:#eef2f1"><iframe src="${String(q.src || '/portal').replace(/"/g, '')}" style="border:0;width:390px;height:${+q.h || 844}px;display:block"></iframe>`);
  }
  if (p === '/api/session') return json(res, { activated: !flags.gate, signedIn: !flags.gate, model: MODEL, serial: SERIAL, fw: '0.4.0', uiLang: 'en' });
  if (p === '/api/status') return json(res, status());
  if (p === '/api/config' && req.method === 'GET') return json(res, config());
  if (p === '/api/image' && req.method === 'GET') return json(res, image());
  if (p === '/api/schedule') return json(res, schedule());
  if (p === '/api/events') return json(res, events());
  if (p === '/api/wifi/scan') return json(res, scan);
  if (p === '/api/logs') return json(res, { text: q.since === '0' || !q.since ? logs : '', next: logs.length, lost: false });
  if (p === '/api/sd') return json(res, { present: true, card: 'SD32G', totalMB: 29820, freeMB: 29102, save: true, days: days.map((d) => ({ day: d, files: files(d).length })) });
  if (p === '/api/sd/files') return json(res, files(q.day));
  if (p === '/api/sd/file') return send(res, 200, 'image/svg+xml', trapSvg(1280, 960, +String(q.name).slice(0, 6), 'illustration'));
  if (p === '/api/stream' || p === '/api/frame.jpg') return send(res, 200, 'image/svg+xml', trapSvg(+q.w || 640, +q.h || 480, 42, 'illustration, not a camera picture'));
  if (p === '/api/qr.svg') return send(res, 200, 'image/svg+xml', qrSvg(q.what || 'claim'));
  if (req.method === 'POST') { req.resume(); return json(res, { ok: true }); }
  json(res, { ok: false, error: 'NOT_FOUND' }, 404);
}).listen(PORT, () => console.log(`mock camera on http://localhost:${PORT}/ (console) and /portal`));
