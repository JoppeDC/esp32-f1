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

/* ── Pending-flag countdown ──────────────────────────────────────────────── */
// The device only pushes a status event when something changes, so the
// countdown to the next queued flag is kept locally from the last snapshot.
let currentMeta = FLAG_META.IDLE;
let pending     = null;   // { label, dueAt } or null
let expired     = false;  // no fresh state: the LEDs are held at IDLE

function renderSub() {
  if (expired) {
    flagSub.textContent = 'No fresh data from relay';
  } else if (pending) {
    const secs = Math.max(0, Math.ceil((pending.dueAt - Date.now()) / 1000));
    flagSub.textContent = `→ ${pending.label} in ${secs} s`;
  } else {
    flagSub.textContent = currentMeta.sub;
  }
}
setInterval(renderSub, 500);

/* ── Apply a status snapshot to the UI ───────────────────────────────────── */
function applyStatus(d) {
  // Flag indicator. While expired the LEDs show IDLE regardless of the queue,
  // so mirror that rather than the queued flag.
  expired = !!d.expired;
  currentMeta = (!expired && FLAG_META[d.flag]) || FLAG_META.IDLE;
  flagIndicator.className = 'flag-indicator ' + currentMeta.css;
  flagName.textContent = currentMeta.label;

  pending = d.pendingFlag
    ? { label: (FLAG_META[d.pendingFlag] || { label: d.pendingFlag }).label,
        dueAt: Date.now() + (d.pendingInMs ?? 0) }
    : null;
  renderSub();

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

/* ── API helpers ─────────────────────────────────────────────────────────── */
// The device refuses state-changing requests without this header (it forces
// a CORS preflight, so another site's page can't reboot the lamp).
const API_HEADERS = { 'Content-Type': 'application/json', 'X-F1-Sensor': '1' };

function apiPost(path, body) {
  return fetch(path, {
    method:  'POST',
    headers: API_HEADERS,
    body:    body === undefined ? undefined : JSON.stringify(body),
  });
}

/* ── Load config from device ─────────────────────────────────────────────── */
// Until the current config has been shown, the relay URL field is empty for
// the wrong reason, and saving would persist that emptiness and disable the
// relay. The submit handler only sends relay_url once this is true.
let configLoaded = false;

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
    configLoaded = true;
  } catch (err) {
    console.warn('Could not load config:', err);
    showFeedback('Could not load settings — reload the page');
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
  };
  if (configLoaded) payload.relay_url = relayUrlInput.value.trim();

  try {
    const res = await apiPost('/api/config', payload);

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
async function deviceAction(path) {
  try {
    await apiPost(path);
    connPill.textContent = 'Restarting…';
    connPill.className   = 'connection-pill';
  } catch {
    showFeedback('Request failed');
  }
}

restartBtn.addEventListener('click', () => {
  if (!confirm('Restart the device?')) return;
  deviceAction('/api/restart');
});

wifiResetBtn.addEventListener('click', () => {
  if (!confirm('This will erase WiFi credentials and open the setup portal on next boot. Continue?')) return;
  deviceAction('/api/wifi/reset');
});

/* ── Init ────────────────────────────────────────────────────────────────── */
connectSSE();
loadConfig();
loadStatus();
