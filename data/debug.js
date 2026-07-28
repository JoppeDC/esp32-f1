/* ── Flag button metadata ──────────────────────────────────────────────────── */
const FLAGS = [
  { key: 'IDLE',   label: 'Idle'       },
  { key: 'CLEAR',  label: 'Clear'      },
  { key: 'YELLOW', label: 'Yellow'     },
  { key: 'VSC',    label: 'VSC'        },
  { key: 'SC',     label: 'Safety Car' },
  { key: 'RED',    label: 'Red Flag'   },
  { key: 'CHEQ',   label: 'Chequered'  },
];

/* ── DOM refs ─────────────────────────────────────────────────────────────── */
const flagBtnGrid    = document.getElementById('flag-buttons');
const overrideBanner = document.getElementById('override-banner');
const overrideName   = document.getElementById('override-flag-name');
const releaseBtn     = document.getElementById('release-btn');

/* ── Build flag buttons ───────────────────────────────────────────────────── */
FLAGS.forEach(f => {
  const btn = document.createElement('button');
  btn.className     = 'flag-btn';
  btn.dataset.flag  = f.key;
  btn.textContent   = f.label;
  btn.addEventListener('click', () => setOverride(f.key));
  flagBtnGrid.appendChild(btn);
});

releaseBtn.addEventListener('click', () => setOverride('LIVE'));

/* ── Override API ─────────────────────────────────────────────────────────── */
async function setOverride(flag) {
  try {
    await fetch('/api/flag/override', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify({ flag }),
    });
    pollLive();   // refresh immediately
  } catch { /* ignore */ }
}

/* ── Format helpers ───────────────────────────────────────────────────────── */
function fmtUptime(ms) {
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const parts = [];
  if (d) parts.push(d + 'd');
  if (h) parts.push(h + 'h');
  if (m) parts.push(m + 'm');
  parts.push(sec + 's');
  return parts.join(' ');
}

function fmtBytes(b) {
  return b.toLocaleString() + ' B';
}

function fmtAgo(ms) {
  if (ms > 86400000) return '—';
  if (ms >= 60000)   return (ms / 60000).toFixed(1) + ' min ago';
  return (ms / 1000).toFixed(1) + 's ago';
}

/* ── Apply system info (called once) ──────────────────────────────────────── */
function applySystem(d) {
  setText('sys-free-heap',   fmtBytes(d.freeHeap));
  setText('sys-min-heap',    fmtBytes(d.minFreeHeap));
  setText('sys-chip',        d.chipModel);
  setText('sys-cpu',         d.cpuFreqMHz + ' MHz');
  setText('sys-sdk',         d.sdkVersion);
  setText('sys-uptime',      fmtUptime(d.uptimeMs));

  const wifi = d.wifi || {};
  setText('wifi-ssid',    wifi.ssid);
  setText('wifi-bssid',   wifi.bssid);
  setText('wifi-channel', wifi.channel);
  setText('wifi-ip',      wifi.ip);
  setText('wifi-gateway', wifi.gateway);
  setText('wifi-subnet',  wifi.subnet);
  setText('wifi-dns',     wifi.dns);
  setText('wifi-rssi',    wifi.rssi != null ? wifi.rssi + ' dBm' : '—');
  setText('wifi-txpower', wifi.txPower != null ? wifi.txPower + ' dBm' : '—');
}

/* ── Apply live data (polled) ─────────────────────────────────────────────── */
function applyLive(d) {
  // Relay
  const rl = d.relay || {};
  setText('relay-state',     rl.state);
  setText('relay-reconnect', rl.reconnectDelayMs + ' ms');
  setText('relay-last-msg',  fmtAgo(rl.lastMessageAgoMs));
  setText('relay-fresh',     fmtAgo(rl.freshAgeMs));
  setText('relay-stale',     rl.stale ? 'yes' : 'no');

  // F1 State
  const f1 = d.f1 || {};
  setText('f1-flag',           f1.flag);
  setText('f1-queue',          f1.queueDepth);
  setText('f1-track-raw',      f1.trackStatusRaw || '—');
  setText('f1-session-type',   f1.sessionType);
  setText('f1-session-status', f1.sessionStatus);
  setText('f1-last-queued',    f1.lastQueuedFlag);

  // LED
  const led = d.led || {};
  setText('led-animation',  led.animation);
  setText('led-brightness', led.brightness + ' / 255');
  setText('led-count',      led.ledCount);
  setText('led-pin',        'GPIO ' + led.dataPin);

  // Override banner + button highlight
  const ovr = d.override || {};
  if (ovr.active) {
    overrideBanner.style.display = '';
    overrideName.textContent     = ovr.flag;
  } else {
    overrideBanner.style.display = 'none';
  }

  document.querySelectorAll('.flag-btn').forEach(btn => {
    btn.classList.toggle('active', ovr.active && btn.dataset.flag === ovr.flag);
  });
}

function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val != null ? val : '—';
}

/* ── Fetch system info once ───────────────────────────────────────────────── */
async function loadSystem() {
  try {
    const res  = await fetch('/api/debug/system');
    applySystem(await res.json());
  } catch { /* ignore */ }
}

/* ── Poll live data ───────────────────────────────────────────────────────── */
async function pollLive() {
  try {
    const res  = await fetch('/api/debug/live');
    applyLive(await res.json());
  } catch { /* ignore */ }
}

loadSystem();
pollLive();
setInterval(pollLive, 2000);
