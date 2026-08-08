#!/usr/bin/env node
//
// claude-usage-bridge
//
// Runs on the machine where Claude Code lives and exposes one JSON endpoint
// that the ESP32-S3 dashboard polls over the LAN.
//
// Two independent data sources:
//
//   1. Plan limits — the real 5h / weekly utilisation that `/usage` shows.
//      Anthropic returns it on the `anthropic-ratelimit-unified-*` response
//      headers of any inference call, so we make one deliberately tiny call
//      (max_tokens: 1) with the Claude Code OAuth token and read the headers.
//      Cached, so at most one probe per `limitsCacheSec`.
//
//   2. Cost and tokens — parsed straight out of the local transcripts in
//      ~/.claude/projects/**/*.jsonl. No network, no credentials.
//
// Zero dependencies: Node 18+ only.
//
import http from 'node:http';
import fs from 'node:fs';
import fsp from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import { execFile } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { Speech, defaultCacheDir, wavInfo, SAMPLE_RATE } from './speech.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CLAUDE_DIR = process.env.CLAUDE_CONFIG_DIR || path.join(os.homedir(), '.claude');
const PROJECTS_DIR = path.join(CLAUDE_DIR, 'projects');

// ---------------------------------------------------------------- config ---

const CONFIG_PATH = path.join(HERE, 'config.json');

const DEFAULT_CONFIG = {
  port: 8787,
  host: '0.0.0.0',
  token: '',
  limitsEnabled: true,
  limitsCacheSec: 60,
  probeModel: 'claude-haiku-4-5-20251001',
  credentialsPath: '',
  // On a 401, exchange the stored refresh token for a fresh access token the
  // way Claude Code does, and write the result back. Turn this off to leave
  // the credentials file strictly read-only (the gauges then stop working
  // whenever the stored access token ages out).
  refreshEnabled: true,
  // Spoken alerts. The device has a speaker but no usable text-to-speech, so
  // the phrase is synthesised here with the OS voice and streamed over.
  speech: {
    enabled: true,
    lang: 'en',   // 'en' | 'es'
    voice: '',    // exact voice name; blank picks the first matching the lang
  },
};

// This token gets read off a console and typed by hand into a tiny form on a
// phone, so the alphabet drops every glyph pair that gets confused in that
// round trip: I/L/1, O/0, U/V. base64url would not do.
const TOKEN_ALPHABET = 'ABCDEFGHJKMNPQRSTWXYZabcdefghjkmnpqrstwxyz23456789';

function generateToken(len = 12) {
  let out = '';
  for (let i = 0; i < len; i++) out += TOKEN_ALPHABET[crypto.randomInt(TOKEN_ALPHABET.length)];
  return out;
}

function loadConfig() {
  let cfg = { ...DEFAULT_CONFIG };
  if (fs.existsSync(CONFIG_PATH)) {
    try {
      cfg = { ...cfg, ...JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8')) };
    } catch (err) {
      console.error(`[config] ${CONFIG_PATH} is not valid JSON: ${err.message}`);
    }
  }
  if (!cfg.token) {
    cfg.token = generateToken();
    fs.writeFileSync(CONFIG_PATH, JSON.stringify(cfg, null, 2));
    console.log(`[config] generated a bridge token and wrote ${CONFIG_PATH}`);
  }
  if (process.env.BRIDGE_PORT) cfg.port = Number(process.env.BRIDGE_PORT);
  if (process.env.BRIDGE_TOKEN) cfg.token = process.env.BRIDGE_TOKEN;
  if (process.env.BRIDGE_LIMITS === '0') cfg.limitsEnabled = false;
  return cfg;
}

const config = loadConfig();
const pricing = JSON.parse(fs.readFileSync(path.join(HERE, 'pricing.json'), 'utf8'));
const speech = new Speech(config.speech, defaultCacheDir(HERE));

// ----------------------------------------------------------- credentials ---

// Public OAuth client id Claude Code itself uses. Not a secret.
const OAUTH_CLIENT_ID = '9d1c250a-e61b-44d9-88ed-5944d1962f5e';
const OAUTH_TOKEN_URL = 'https://console.anthropic.com/v1/oauth/token';

function credentialsFile() {
  return config.credentialsPath || path.join(CLAUDE_DIR, '.credentials.json');
}

// Claude Code stores its OAuth token in ~/.claude/.credentials.json on Windows
// and Linux, and in the login keychain on macOS. We only ever read it, and it
// only ever leaves this process inside the Authorization header of the probe.
async function readOAuthToken() {
  if (process.env.CLAUDE_CODE_OAUTH_TOKEN) {
    return { token: process.env.CLAUDE_CODE_OAUTH_TOKEN, refreshToken: '', plan: '', expiresAt: 0, source: 'env' };
  }

  const file = credentialsFile();
  if (fs.existsSync(file)) {
    const raw = JSON.parse(await fsp.readFile(file, 'utf8'));
    const o = raw.claudeAiOauth || raw;
    if (o?.accessToken) {
      return {
        token: o.accessToken,
        refreshToken: o.refreshToken || '',
        plan: o.subscriptionType || '',
        expiresAt: o.expiresAt || 0,
        source: 'file',
      };
    }
  }

  if (process.platform === 'darwin') {
    const out = await new Promise((resolve) => {
      execFile(
        'security',
        ['find-generic-password', '-s', 'Claude Code-credentials', '-w'],
        (err, stdout) => resolve(err ? null : stdout.trim()),
      );
    });
    if (out) {
      const o = JSON.parse(out).claudeAiOauth || {};
      if (o.accessToken) {
        return {
          token: o.accessToken,
          refreshToken: o.refreshToken || '',
          plan: o.subscriptionType || '',
          expiresAt: o.expiresAt || 0,
          source: 'keychain',
        };
      }
    }
  }

  throw new Error('no Claude Code credentials found');
}

// Exchanges the refresh token for a new access token and persists the result,
// because Anthropic rotates the refresh token on use: keeping the new one only
// in memory would strand the copy on disk. The original file is backed up once
// to `.credentials.json.bak` before the first rewrite, and the replacement is
// written to a temp file and renamed so a crash can never leave a half-written
// credentials file behind.
async function refreshAccessToken(refreshToken) {
  const res = await fetch(OAUTH_TOKEN_URL, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      grant_type: 'refresh_token',
      refresh_token: refreshToken,
      client_id: OAUTH_CLIENT_ID,
    }),
  });

  if (!res.ok) throw new Error(`refresh failed (HTTP ${res.status})`);
  const body = await res.json();
  if (!body.access_token) throw new Error('refresh returned no access token');

  const file = credentialsFile();
  if (fs.existsSync(file)) {
    const backup = `${file}.bak`;
    if (!fs.existsSync(backup)) await fsp.copyFile(file, backup);

    const raw = JSON.parse(await fsp.readFile(file, 'utf8'));
    const holder = raw.claudeAiOauth ? raw.claudeAiOauth : raw;
    holder.accessToken = body.access_token;
    if (body.refresh_token) holder.refreshToken = body.refresh_token;
    if (body.expires_in) holder.expiresAt = Date.now() + body.expires_in * 1000;

    const tmp = `${file}.tmp`;
    await fsp.writeFile(tmp, JSON.stringify(raw, null, 2), { mode: 0o600 });
    await fsp.rename(tmp, file);
  }

  console.log('[auth] access token refreshed');
  return body.access_token;
}

// --------------------------------------------------------------- limits ----

let limitsCache = { at: 0, value: null };

// The header names have shifted over time (`anthropic-ratelimit-unified-5h-*`
// today), so match on shape rather than on an exact string.
function readWindowHeaders(headers, windowKey) {
  let utilization = null;
  let reset = null;
  let status = '';

  for (const [rawName, rawValue] of headers.entries()) {
    const name = rawName.toLowerCase();
    if (!name.includes('ratelimit') || !name.includes(windowKey)) continue;
    // Skip the model-scoped variants when reading the generic window.
    if (windowKey === '7d' && name.includes('opus')) continue;

    if (name.endsWith('utilization')) utilization = rawValue;
    else if (name.endsWith('reset')) reset = rawValue;
    // Anthropic reports per-window status too, so exhaustion never has to be
    // inferred from a percentage threshold we invented.
    else if (name.endsWith('status')) status = rawValue;
  }

  if (utilization === null) return null;

  let pct = Number(utilization);
  if (!Number.isFinite(pct)) return null;
  // Observed format is a 0..1 fraction ("0.16" = 16% consumed), so 1.0 means
  // the window is spent. Anything above 1 is taken as an already-scaled
  // percentage, which keeps this working if the format ever changes.
  if (pct <= 1) pct *= 100;
  pct = Math.max(0, Math.min(100, pct));

  return { pct: Number(pct.toFixed(1)), resetInSec: parseReset(reset), status };
}

function readOpusWindow(headers) {
  let utilization = null;
  let reset = null;
  for (const [rawName, rawValue] of headers.entries()) {
    const name = rawName.toLowerCase();
    if (!name.includes('ratelimit') || !name.includes('opus')) continue;
    if (name.endsWith('utilization')) utilization = rawValue;
    else if (name.endsWith('reset')) reset = rawValue;
  }
  if (utilization === null) return null;
  let pct = Number(utilization);
  if (!Number.isFinite(pct)) return null;
  if (pct <= 1) pct *= 100;
  return { pct: Number(Math.max(0, Math.min(100, pct)).toFixed(1)), resetInSec: parseReset(reset) };
}

// Resets arrive either as a unix timestamp or as an ISO 8601 instant.
function parseReset(raw) {
  if (!raw) return 0;
  const asNumber = Number(raw);
  const targetMs = Number.isFinite(asNumber)
    ? (asNumber > 1e12 ? asNumber : asNumber * 1000)
    : Date.parse(raw);
  if (!Number.isFinite(targetMs)) return 0;
  return Math.max(0, Math.round((targetMs - Date.now()) / 1000));
}

// The smallest inference call that still gets the rate-limit headers back.
async function sendProbe(token) {
  const res = await fetch('https://api.anthropic.com/v1/messages', {
    method: 'POST',
    headers: {
      authorization: `Bearer ${token}`,
      'anthropic-version': '2023-06-01',
      'anthropic-beta': 'oauth-2025-04-20',
      'content-type': 'application/json',
      'user-agent': 'claude-usage-bridge/1.0',
    },
    body: JSON.stringify({
      model: config.probeModel,
      max_tokens: 1,
      // Claude Code OAuth tokens require this exact system preamble.
      system: "You are Claude Code, Anthropic's official CLI for Claude.",
      messages: [{ role: 'user', content: 'ping' }],
    }),
  });

  if (process.env.BRIDGE_DEBUG) {
    const pairs = [...res.headers.entries()]
      .filter(([n]) => n.includes('ratelimit'))
      .map(([n, v]) => `${n}=${v}`);
    console.error(`[probe] HTTP ${res.status}`);
    for (const p of pairs) console.error(`[probe]   ${p}`);
    if (!res.ok) console.error(`[probe] body: ${(await res.clone().text()).slice(0, 300)}`);
  }
  return res;
}

async function probeLimits() {
  const now = Date.now();
  if (limitsCache.value && now - limitsCache.at < config.limitsCacheSec * 1000) {
    return limitsCache.value;
  }

  const result = { ok: false, status: '', plan: '', error: '', session: null, week: null, weekOpus: null };

  try {
    const creds = await readOAuthToken();
    result.plan = creds.plan;

    let res = await sendProbe(creds.token);

    // The stored access token ages out roughly daily, so a 401 here is normal
    // rather than fatal: swap it for a fresh one and try once more.
    if ((res.status === 401 || res.status === 403) && config.refreshEnabled && creds.refreshToken) {
      const fresh = await refreshAccessToken(creds.refreshToken);
      res = await sendProbe(fresh);
    }

    result.session = readWindowHeaders(res.headers, '5h');
    result.week = readWindowHeaders(res.headers, '7d');
    result.weekOpus = readOpusWindow(res.headers);
    result.status = res.headers.get('anthropic-ratelimit-unified-status') || '';

    if (res.status === 401 || res.status === 403) {
      throw new Error('re-authenticate: run claude /login');
    }
    if (!result.session && !result.week) {
      throw new Error(res.ok ? 'no rate-limit headers' : `HTTP ${res.status}`);
    }
    result.ok = true;
    await evaluateAlerts(result);
  } catch (err) {
    result.error = String(err.message || err).slice(0, 46);
  }

  limitsCache = { at: now, value: result };
  return result;
}

// --------------------------------------------------------------- alerts ----
//
// Only two things are worth interrupting someone for: a window running out,
// and that window coming back. Both are read from Anthropic's own per-window
// status rather than from a percentage threshold of our invention, with the
// utilisation as a fallback for the case where the header is absent.
//

const PHRASES = {
  en: {
    session: '5 hour limit',
    week: 'weekly limit',
    exhausted: (what, when) => `${what} reached.${when ? ` Resets in ${when}.` : ''}`,
    recovered: (what) => `${what} has reset. You are good to go.`,
    dur: (h, m) =>
      [h ? `${h} hour${h === 1 ? '' : 's'}` : '', m ? `${m} minute${m === 1 ? '' : 's'}` : '']
        .filter(Boolean)
        .join(' '),
  },
  es: {
    session: 'el límite de 5 horas',
    week: 'el límite semanal',
    exhausted: (what, when) =>
      `Se agotó ${what}.${when ? ` Se restablece en ${when}.` : ''}`,
    recovered: (what) => `Se restableció ${what}. Ya podés seguir.`,
    dur: (h, m) =>
      [h ? `${h} hora${h === 1 ? '' : 's'}` : '', m ? `${m} minuto${m === 1 ? '' : 's'}` : '']
        .filter(Boolean)
        .join(' y '),
  },
};

const alertState = {
  seq: 0,
  current: null,          // { id, kind, window, text, wavPath }
  exhausted: { session: null, week: null },
};

function isExhausted(w) {
  if (!w) return null;
  const status = (w.status || '').toLowerCase();
  if (status === 'rejected') return true;
  if (status === 'allowed' || status === 'allowed_warning') return w.pct >= 99.5;
  return w.pct >= 99.5;
}

function spokenDuration(sec, strings) {
  if (!sec) return '';
  const h = Math.floor(sec / 3600);
  const m = Math.round((sec % 3600) / 60);
  return strings.dur(h, m);
}

async function raiseAlert(kind, windowKey, w) {
  const strings = PHRASES[config.speech.lang] || PHRASES.en;
  const what = strings[windowKey];
  const text =
    kind === 'exhausted'
      ? strings.exhausted(what, spokenDuration(w?.resetInSec, strings))
      : strings.recovered(what);

  const alert = { id: ++alertState.seq, kind, window: windowKey, text, wavPath: null };
  alertState.current = alert;
  console.log(`[alert] ${kind} ${windowKey}: ${text}`);

  if (config.speech.enabled) {
    try {
      alert.wavPath = await speech.synthesize(text);
    } catch (err) {
      console.error(`[alert] ${err.message}`);
    }
  }
}

// Called on every successful probe. The first observation only seeds the
// baseline, so restarting the bridge never fires a spurious alert.
async function evaluateAlerts(limits) {
  for (const key of ['session', 'week']) {
    const w = limits[key];
    const now = isExhausted(w);
    if (now === null) continue;

    const before = alertState.exhausted[key];
    alertState.exhausted[key] = now;
    if (before === null) continue;

    if (now && !before) await raiseAlert('exhausted', key, w);
    else if (!now && before) await raiseAlert('recovered', key, w);
  }
}

// ------------------------------------------------------- transcript costs ---

const fileState = new Map(); // path -> { offset, size, tail }
const byDay = new Map();     // 'YYYY-MM-DD' -> { cost, in, out, cr, cw, models: Map }
const seen = new Set();      // dedupe key per assistant message
const recent = [];           // { ts, cost } inside the last 5 hours
let lastActivityMs = 0;

function priceFor(model) {
  const id = (model || '').toLowerCase();
  let best = null;
  let bestLen = 0;
  for (const [key, value] of Object.entries(pricing)) {
    if (key.startsWith('_')) continue;
    if (id.includes(key) && key.length > bestLen) {
      best = value;
      bestLen = key.length;
    }
  }
  return best;
}

// "claude-opus-5-20260101" -> "Opus 5", "claude-haiku-4-5-..." -> "Haiku 4.5".
// The lookahead stops the version from swallowing the trailing date stamp.
function prettyModel(model) {
  const id = (model || 'unknown').toLowerCase();
  const m = id.match(/(opus|sonnet|haiku)-?(\d{1,2}(?:[-.]\d{1,2})?)?(?!\d)/);
  if (!m) return String(model).slice(0, 18);
  const family = m[1][0].toUpperCase() + m[1].slice(1);
  const version = (m[2] || '').replace('-', '.');
  return version ? `${family} ${version}` : family;
}

function dayKey(ms) {
  const d = new Date(ms);
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`;
}

function bucket(key) {
  let b = byDay.get(key);
  if (!b) {
    b = { cost: 0, in: 0, out: 0, cr: 0, cw: 0, models: new Map() };
    byDay.set(key, b);
  }
  return b;
}

function ingestLine(line) {
  if (!line || line[0] !== '{') return;

  let entry;
  try {
    entry = JSON.parse(line);
  } catch {
    return;
  }

  const usage = entry?.message?.usage;
  if (!usage || entry.type !== 'assistant') return;

  // Claude Code rewrites assistant messages as tool calls resolve, so the same
  // message id shows up repeatedly. Count it once.
  const id = `${entry.message.id || ''}:${entry.requestId || ''}`;
  if (id === ':' || seen.has(id)) return;
  seen.add(id);

  const ts = Date.parse(entry.timestamp || '') || Date.now();
  const model = entry.message.model || 'unknown';
  const price = priceFor(model);
  if (!price) return; // synthetic entries (e.g. <synthetic> model) carry no price

  const tIn = usage.input_tokens || 0;
  const tOut = usage.output_tokens || 0;
  const tCr = usage.cache_read_input_tokens || 0;
  const tCw = usage.cache_creation_input_tokens || 0;

  const cost =
    (tIn * price.input + tOut * price.output + tCr * price.cacheRead + tCw * price.cacheWrite) / 1e6;

  const b = bucket(dayKey(ts));
  b.cost += cost;
  b.in += tIn;
  b.out += tOut;
  b.cr += tCr;
  b.cw += tCw;

  const label = prettyModel(model);
  b.models.set(label, (b.models.get(label) || 0) + cost);

  recent.push({ ts, cost });
  if (ts > lastActivityMs) lastActivityMs = ts;
}

async function* walkJsonl(dir) {
  let entries;
  try {
    entries = await fsp.readdir(dir, { withFileTypes: true });
  } catch {
    return;
  }
  for (const e of entries) {
    const full = path.join(dir, e.name);
    if (e.isDirectory()) yield* walkJsonl(full);
    else if (e.isFile() && e.name.endsWith('.jsonl')) yield full;
  }
}

// Only the bytes appended since the previous scan are read, so a busy
// transcript directory stays cheap to poll.
async function scanTranscripts() {
  for await (const file of walkJsonl(PROJECTS_DIR)) {
    let stat;
    try {
      stat = await fsp.stat(file);
    } catch {
      continue;
    }

    const state = fileState.get(file) || { offset: 0, size: 0, tail: '' };
    if (stat.size < state.offset) {
      // Truncated or rotated — start over on this file.
      state.offset = 0;
      state.tail = '';
    }
    if (stat.size === state.offset) continue;

    let handle;
    try {
      handle = await fsp.open(file, 'r');
      const length = stat.size - state.offset;
      const buf = Buffer.allocUnsafe(length);
      await handle.read(buf, 0, length, state.offset);

      const text = state.tail + buf.toString('utf8');
      const lines = text.split('\n');
      state.tail = lines.pop() ?? ''; // keep the partial last line for next time
      for (const line of lines) ingestLine(line.trim());

      state.offset = stat.size;
      state.size = stat.size;
      fileState.set(file, state);
    } catch {
      // Locked or vanished mid-scan; try again on the next poll.
    } finally {
      await handle?.close();
    }
  }

  // Keep memory bounded: 40 days of buckets and a 5h tail of entries.
  const cutoff = Date.now() - 40 * 86400_000;
  for (const key of byDay.keys()) {
    if (Date.parse(key) < cutoff) byDay.delete(key);
  }
  // Files are visited in directory order, so `recent` is not sorted by
  // timestamp — compact it in place rather than shifting off the front.
  const fiveHoursAgo = Date.now() - 5 * 3600_000;
  let kept = 0;
  for (const e of recent) if (e.ts >= fiveHoursAgo) recent[kept++] = e;
  recent.length = kept;

  // Dedupe only has to hold within a scan window — we never re-read bytes we
  // already consumed — so dropping the set when it gets large is safe.
  if (seen.size > 300_000) seen.clear();
}

const DAY_NAMES = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

function buildCost() {
  const now = Date.now();
  const todayKey = dayKey(now);
  const today = byDay.get(todayKey);

  const days = [];
  for (let i = 6; i >= 0; i--) {
    const ms = now - i * 86400_000;
    const b = byDay.get(dayKey(ms));
    days.push({
      label: DAY_NAMES[new Date(ms).getDay()],
      usd: Number((b?.cost || 0).toFixed(2)),
    });
  }

  const monthPrefix = todayKey.slice(0, 7);
  let monthUsd = 0;
  for (const [key, b] of byDay) if (key.startsWith(monthPrefix)) monthUsd += b.cost;

  const todayTotal = today?.cost || 0;
  const models = [...(today?.models || new Map())]
    .sort((a, b) => b[1] - a[1])
    .slice(0, 4)
    .map(([name, usd]) => ({
      name,
      usd: Number(usd.toFixed(2)),
      pct: todayTotal > 0 ? Number(((usd / todayTotal) * 100).toFixed(1)) : 0,
    }));

  return {
    ok: true,
    todayUsd: Number(todayTotal.toFixed(2)),
    monthUsd: Number(monthUsd.toFixed(2)),
    sessionUsd: Number(recent.reduce((sum, e) => sum + e.cost, 0).toFixed(2)),
    days,
    tokens: {
      in: today?.in || 0,
      out: today?.out || 0,
      cacheRead: today?.cr || 0,
      cacheWrite: today?.cw || 0,
    },
    models,
    lastActivitySec: lastActivityMs ? Math.round((now - lastActivityMs) / 1000) : 0,
  };
}

// ----------------------------------------------------------------- server ---

async function buildPayload() {
  const [limits] = await Promise.all([
    config.limitsEnabled
      ? probeLimits()
      : Promise.resolve({ ok: false, error: 'limits disabled', session: null, week: null, weekOpus: null, plan: '', status: '' }),
    scanTranscripts(),
  ]);

  let cost;
  try {
    cost = buildCost();
  } catch (err) {
    cost = { ok: false, error: String(err.message).slice(0, 46) };
  }

  const now = new Date();
  const a = alertState.current;

  return {
    ok: true,
    generatedAt: Math.floor(Date.now() / 1000),
    plan: limits.plan || '',
    limits: {
      ok: limits.ok,
      status: limits.status || '',
      error: limits.error || '',
      session: limits.session,
      week: limits.week,
      weekOpus: limits.weekOpus,
    },
    cost,
    // Wall-clock minutes since local midnight. The device has no RTC and no
    // timezone, so quiet hours are evaluated against this rather than making
    // the firmware do NTP and DST.
    clock: { localMinutes: now.getHours() * 60 + now.getMinutes() },
    alert: a
      ? { id: a.id, kind: a.kind, window: a.window, text: a.text, hasAudio: !!a.wavPath }
      : { id: 0, kind: '', window: '', text: '', hasAudio: false },
  };
}

function authorized(req) {
  if (!config.token) return true;
  const header = req.headers.authorization || '';
  const supplied = header.startsWith('Bearer ') ? header.slice(7) : '';
  if (supplied.length !== config.token.length) return false;
  return crypto.timingSafeEqual(Buffer.from(supplied), Buffer.from(config.token));
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://localhost');

  if (url.pathname === '/health') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ ok: true, version: '1.0.0' }));
    return;
  }

  // Raises a synthetic alert so the device speaks on the next poll. Useful for
  // checking the speaker and the quiet-hours window without waiting to
  // actually run out of quota.
  if (url.pathname === '/test-alert') {
    if (!authorized(req)) return void res.writeHead(401).end('unauthorized');
    const kind = url.searchParams.get('kind') === 'recovered' ? 'recovered' : 'exhausted';
    const win = url.searchParams.get('window') === 'week' ? 'week' : 'session';
    await raiseAlert(kind, win, { resetInSec: 9300 });
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ ok: true, alert: { id: alertState.seq, kind, window: win, text: alertState.current.text } }));
    return;
  }

  if (!['/usage', '/alert.pcm', '/speak'].includes(url.pathname)) {
    res.writeHead(404).end('not found');
    return;
  }

  if (!authorized(req)) {
    res.writeHead(401, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ ok: false, error: 'unauthorized' }));
    return;
  }

  // Raw signed 16-bit little-endian mono samples, header already stripped.
  // Parsing RIFF chunks belongs here, where the whole buffer is in hand, not
  // in the firmware's I2S path — SAPI's 18-byte `fmt ` chunk puts the samples
  // at offset 46, and a firmware that assumed 44 would play noise.
  if (url.pathname === '/alert.pcm' || url.pathname === '/speak') {
    try {
      let wavPath;
      if (url.pathname === '/speak') {
        const text = url.searchParams.get('text');
        if (!text) return void res.writeHead(400).end('missing text');
        wavPath = await speech.synthesize(text.slice(0, 300));
      } else {
        if (!alertState.current?.wavPath) return void res.writeHead(404).end('no alert audio');
        wavPath = alertState.current.wavPath;
      }

      const buf = await fsp.readFile(wavPath);
      const info = wavInfo(buf);
      if (!info) return void res.writeHead(500).end('unreadable wav');

      const pcm = buf.subarray(info.dataOffset, info.dataOffset + info.dataSize);
      res.writeHead(200, {
        'content-type': 'application/octet-stream',
        'content-length': pcm.length,
        'x-sample-rate': String(info.sampleRate),
        'x-channels': String(info.channels),
        'x-bits': String(info.bits),
      });
      res.end(pcm);
    } catch (err) {
      res.writeHead(500, { 'content-type': 'text/plain' });
      res.end(String(err.message).slice(0, 200));
    }
    return;
  }

  try {
    const payload = await buildPayload();
    const body = JSON.stringify(payload);
    res.writeHead(200, { 'content-type': 'application/json', 'content-length': Buffer.byteLength(body) });
    res.end(body);
  } catch (err) {
    res.writeHead(500, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ ok: false, error: String(err.message) }));
  }
});

// Only listen when run directly, so the parsers above can be unit-tested by
// importing this file.
const runDirectly =
  process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));

if (runDirectly) {
  server.listen(config.port, config.host, () => {
    const nets = Object.values(os.networkInterfaces()).flat();
    const lan = nets.find((n) => n && n.family === 'IPv4' && !n.internal);
    console.log(`claude-usage-bridge listening on ${config.host}:${config.port}`);
    console.log(`  transcripts : ${PROJECTS_DIR}`);
    console.log(`  limits      : ${config.limitsEnabled ? `on (probe every ${config.limitsCacheSec}s)` : 'off'}`);
    console.log('');
    console.log('  Point the device at:');
    console.log(`    host  ${lan ? lan.address : '<this machine IP>'}`);
    console.log(`    port  ${config.port}`);
    console.log(`    token ${config.token}`);
  });
}

export { readWindowHeaders, readOpusWindow, parseReset, prettyModel, priceFor };
