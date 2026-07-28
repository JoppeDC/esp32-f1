/* ── DOM refs ─────────────────────────────────────────────────────────────── */
const connPill        = document.getElementById('conn-pill');
const flagIndicator   = document.getElementById('flag-indicator');
const flagName        = document.getElementById('flag-name');
const flagSub         = document.getElementById('flag-sub');
const statSession     = document.getElementById('stat-session');
const statSessionSt   = document.getElementById('stat-session-status');
const statTrackRaw    = document.getElementById('stat-track-raw');
const statRelay       = document.getElementById('stat-relay');
const statIP          = document.getElementById('stat-ip');
const statRSSI        = document.getElementById('stat-rssi');

const ledCountInput   = document.getElementById('led-count');
const brightnessInput = document.getElementById('brightness');
const brightnessVal   = document.getElementById('brightness-val');
const delayInput      = document.getElementById('delay');
const delayVal        = document.getElementById('delay-val');
const relayUrlInput   = document.getElementById('relay-url');
const settingsForm    = document.getElementById('settings-form');
const saveFeedback    = document.getElementById('save-feedback');

const restartBtn      = document.getElementById('restart-btn');
const wifiResetBtn    = document.getElementById('wifi-reset-btn');

/* ── Flag metadata ───────────────────────────────────────────────────────── */
const FLAG_META = {
  IDLE:   { label: 'Idle',         sub: 'No active session',      css: 'IDLE'   },
  CLEAR:  { label: 'Green Flag',   sub: 'Track clear',            css: 'CLEAR'  },
  YELLOW: { label: 'Yellow Flag',  sub: 'Caution on track',       css: 'YELLOW' },
  VSC:    { label: 'Virtual SC',   sub: 'Virtual Safety Car',     css: 'VSC'    },
  SC:     { label: 'Safety Car',   sub: 'Safety car deployed',    css: 'SC'     },
  RED:    { label: 'Red Flag',     sub: 'Session suspended',      css: 'RED'    },
  CHEQ:   { label: 'Chequered',    sub: 'Session finished',       css: 'CHEQ'   },
};

/* ── Relay connection states (see RelayClient::getStateStr) ──────────────── */
const RELAY_META = {
  connected:    { pill: 'Connected',     cls: 'connection-pill connected' },
  reconnecting: { pill: 'Reconnecting…', cls: 'connection-pill'           },
  unconfigured: { pill: 'No relay',      cls: 'connection-pill error'     },
  invalid:      { pill: 'Bad relay URL', cls: 'connection-pill error'     },
  incompatible: { pill: 'Incompatible',  cls: 'connection-pill error'     },
};

/* ── Apply a status snapshot to the UI ───────────────────────────────────── */
function applyStatus(d) {
  // Flag indicator
  const meta = FLAG_META[d.flag] || FLAG_META['IDLE'];
  flagIndicator.className = 'flag-indicator ' + meta.css;
  flagName.textContent = meta.label;

  // Sub-label: show "pending" info while delay is counting down
  flagSub.textContent = d.pendingFlag
    ? `→ ${(FLAG_META[d.pendingFlag] || { label: d.pendingFlag }).label} in ${Math.round(d.delayMs / 1000)} s`
    : meta.sub;

  // Stats
  statSession.textContent    = d.session     || '—';
  statSessionSt.textContent  = d.sessionStatus || '—';
  statTrackRaw.textContent   = d.trackRaw    || '—';
  statIP.textContent         = d.ip          || '—';
  statRSSI.textContent       = d.rssi != null ? `${d.rssi} dBm` : '—';

  // Relay: "stale" means the relay is up but its own F1 link is down, so the
  // values above are last-known rather than current.
  statRelay.textContent = d.relayState
    ? (d.stale ? `${d.relayState} · stale` : d.relayState)
    : '—';

  // Header pill
  const relayMeta = RELAY_META[d.relayState];
  if (relayMeta) {
    connPill.textContent = d.stale && d.relayState === 'connected'
      ? 'Connected · stale'
      : relayMeta.pill;
    connPill.className = relayMeta.cls;
  } else {
    connPill.textContent = 'Offline';
    connPill.className   = 'connection-pill error';
  }
}

/* ── SSE connection ───────────────────────────────────────────────────────── */
function connectSSE() {
  const source = new EventSource('/events');

  source.addEventListener('status', (e) => {
    try { applyStatus(JSON.parse(e.data)); }
    catch { /* ignore malformed */ }
  });

  source.onerror = () => {
    connPill.textContent = 'Reconnecting…';
    connPill.className   = 'connection-pill';
    // EventSource reconnects automatically; no manual retry needed
  };
}

/* ── Load config from device ─────────────────────────────────────────────── */
async function loadConfig() {
  try {
    const res  = await fetch('/api/config');
    const data = await res.json();
    ledCountInput.value   = data.led_count  ?? 60;
    brightnessInput.value = data.brightness ?? 128;
    brightnessVal.textContent = brightnessInput.value;

    const delaySec = Math.round((data.delay_ms ?? 45000) / 1000);
    delayInput.value      = delaySec;
    delayVal.textContent  = delaySec + ' s';

    relayUrlInput.value = data.relay_url ?? '';
  } catch (err) {
    console.warn('Could not load config:', err);
  }
}

/* ── Fetch one-shot status (fallback before first SSE event) ─────────────── */
async function loadStatus() {
  try {
    const res = await fetch('/api/status');
    applyStatus(await res.json());
  } catch { /* ignore */ }
}

/* ── Slider live preview ──────────────────────────────────────────────────── */
brightnessInput.addEventListener('input', () => {
  brightnessVal.textContent = brightnessInput.value;
});

delayInput.addEventListener('input', () => {
  delayVal.textContent = delayInput.value + ' s';
});

/* ── Save settings ───────────────────────────────────────────────────────── */
settingsForm.addEventListener('submit', async (e) => {
  e.preventDefault();

  const payload = {
    led_count:  parseInt(ledCountInput.value,   10),
    brightness: parseInt(brightnessInput.value, 10),
    delay_ms:   parseInt(delayInput.value,      10) * 1000,
    relay_url:  relayUrlInput.value.trim(),
  };

  try {
    const res = await fetch('/api/config', {
      method:  'POST',
      headers: { 'Content-Type': 'application/json' },
      body:    JSON.stringify(payload),
    });

    if (res.ok) {
      showFeedback('Saved');
    } else {
      // The device rejects a malformed relay_url before applying anything,
      // and explains why — show that rather than a generic failure.
      let msg = 'Error saving';
      try { msg = (await res.json()).error || msg; } catch { /* keep default */ }
      showFeedback(msg);
    }
  } catch {
    showFeedback('Request failed');
  }
});

function showFeedback(msg) {
  saveFeedback.textContent = msg;
  saveFeedback.classList.add('visible');
  setTimeout(() => saveFeedback.classList.remove('visible'), 2500);
}

/* ── Device actions ──────────────────────────────────────────────────────── */
restartBtn.addEventListener('click', async () => {
  if (!confirm('Restart the device?')) return;
  await fetch('/api/restart', { method: 'POST' });
  connPill.textContent = 'Restarting…';
  connPill.className   = 'connection-pill';
});

wifiResetBtn.addEventListener('click', async () => {
  if (!confirm('This will erase WiFi credentials and open the setup portal on next boot. Continue?')) return;
  await fetch('/api/wifi/reset', { method: 'POST' });
  connPill.textContent = 'Restarting…';
  connPill.className   = 'connection-pill';
});

/* ── Init ────────────────────────────────────────────────────────────────── */
connectSSE();
loadConfig();
loadStatus();
