# bv-Esp32-S3-claude

A desk dashboard for your Claude plan usage, running on a **Waveshare
ESP32-S3-Touch-LCD-1.54**. Four swipeable screens on a 240×240 IPS panel:
the real 5-hour and weekly limit gauges, spend, tokens by model, and device
status. Dark only, Anthropic palette, Claude Code starburst drawn on the glass.

```
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ ✳ PLAN         ● │  │ ✳ COST         ● │  │ ✳ TOKENS       ● │  │ ✳ SYSTEM       ● │
│      ╭────╮      │  │ TODAY     MONTH  │  │ INPUT   OUTPUT   │  │ WI-FI    casa    │
│     ╱  42 ╲%     │  │ $16.58   $146    │  │ 3.1k    99.4k    │  │ IP    10.0.0.42  │
│    │ 5H WIN │    │  │ $16.58 this ses. │  │ CACHE R CACHE W  │  │ BRIDGE  ok 3s    │
│     ╲______╱     │  │  ▁ ▃ █ ▁ ▂ ▁ ▅   │  │ 4.1M    152k     │  │ BATTERY 88% USB  │
│  resets in 3h 4m │  │  Sa Su Mo Tu We  │  │ BY MODEL         │  │ UPTIME  2h 11m   │
│ ┌──────────────┐ │  │                  │  │ ▓▓▓▓▓ Opus 5     │  │                  │
│ │WEEKLY    61% │ │  │                  │  │ ▓▓ Sonnet 5      │  │                  │
│ │████████░░░░░ │ │  │                  │  │                  │  │                  │
│ └──────────────┘ │  │                  │  │                  │  │                  │
│    ● ○ ○ ○       │  │    ○ ● ○ ○       │  │    ○ ○ ● ○       │  │    ○ ○ ○ ●       │
└──────────────────┘  └──────────────────┘  └──────────────────┘  └──────────────────┘
```

---

## How it works

```
   your PC                                     the device
┌───────────────────────────────┐          ┌──────────────────────┐
│ bridge/bridge.mjs  (Node 18+) │          │ ESP32-S3 + ST7789    │
│                               │          │                      │
│  ~/.claude/projects/**.jsonl ─┼─ cost    │  Wi-Fi ── HTTP GET ──┼──┐
│                               │  tokens  │  LVGL 9 UI           │  │
│  api.anthropic.com headers ───┼─ 5h %    │  CST816 touch        │  │
│    (one tiny probe / 60 s)    │  week %  └──────────────────────┘  │
│                               │                                    │
│         GET /usage  ◄──────────────────────────────────────────────┘
└───────────────────────────────┘
```

The device never talks to Anthropic. It only polls the bridge on your LAN.

### Where the numbers come from

| Screen value | Source |
| --- | --- |
| 5-hour window %, weekly %, Opus weekly %, reset countdowns | `anthropic-ratelimit-unified-*` response headers — the same numbers `/usage` shows |
| Spend today / month / session, 7-day chart, tokens, per-model split | `~/.claude/projects/**/*.jsonl` transcripts, priced with `bridge/pricing.json` |

Anthropic returns the rate-limit headers on *every* inference call but does not
expose them to hooks or the status line, so the bridge makes one deliberately
minimal call (`max_tokens: 1`) using the Claude Code OAuth token and reads the
headers off the response. It is cached, so at most one probe per
`limitsCacheSec` (default 60 s) no matter how often the device polls.

The headers that come back, verified against a live response:

```
anthropic-ratelimit-unified-5h-utilization      0.16     <- fraction, not percent
anthropic-ratelimit-unified-5h-reset            1786208400
anthropic-ratelimit-unified-5h-status           allowed
anthropic-ratelimit-unified-7d-utilization      0.21
anthropic-ratelimit-unified-7d-reset            1786276800
anthropic-ratelimit-unified-status              allowed
anthropic-ratelimit-unified-representative-claim five_hour   <- binding window
anthropic-ratelimit-unified-overage-utilization 0.0
anthropic-ratelimit-unified-fallback-percentage 0.5
```

Utilisation is a `0..1` fraction and resets are unix seconds. Max plans also
send a `-7d-opus-` pair, which the bridge exposes as `weekOpus`; on Pro it is
simply absent and the device shows the plan name instead. The parser matches on
header *shape* rather than exact strings, so a rename does not break it. The
`representative-claim`, `overage-*` and `fallback-percentage` values are passed
over today — they are there if you want to extend the PLAN screen.

**Credential handling.** The token is read from `~/.claude/.credentials.json`
(or the login keychain on macOS, or `CLAUDE_CODE_OAUTH_TOKEN`) and is only ever
sent to `api.anthropic.com` in the `Authorization` header — the same place
Claude Code itself sends it. It is never logged, and never appears in the
`/usage` payload. Set `"limitsEnabled": false` to skip the probe entirely; you
keep the cost screens and the device never needs credentials at all.

**Token refresh.** Stored access tokens age out about once a day, and the
Claude Code desktop app does not keep `.credentials.json` current — so on a 401
the bridge exchanges the stored refresh token for a new access token, exactly
as Claude Code does. Anthropic rotates the refresh token on use, so the result
has to be written back or the copy on disk is stranded. Before the first
rewrite the original is copied to `.credentials.json.bak`, and the replacement
is written to a temp file and renamed, so an interrupted write can never leave
a partial credentials file. Set `"refreshEnabled": false` to keep the file
strictly read-only; the gauges then go stale whenever the access token expires
and you re-authenticate with `claude` yourself.

Run with `BRIDGE_DEBUG=1` to log the probe's HTTP status, which rate-limit
headers came back, and the error body on failure.

---

## Hardware

**Waveshare ESP32-S3-Touch-LCD-1.54** — ESP32-S3R8, 16 MB flash, 8 MB OPI PSRAM,
240×240 ST7789 over 4-wire SPI, CST816 capacitive touch, QMI8658 IMU, ES8311
codec, battery connector.

Pin map (`include/board_config.h`), cross-checked against Waveshare's own
Arduino demo and Clawdmeter's hardware-tested board profile:

| Function | GPIO | | Function | GPIO |
| --- | --- | --- | --- | --- |
| LCD SCLK | 38 | | Touch SDA | 42 |
| LCD MOSI | 39 | | Touch SCL | 41 |
| LCD CS | 21 | | Touch INT | 48 |
| LCD DC | 45 | | Touch RST | 47 |
| LCD RST | 40 | | Battery ADC | 1 |
| Backlight | 46 | | `BAT_EN` latch | 2 |

`BAT_EN` is a power-hold line, not a status pin: it is driven HIGH first thing
in `board_begin()` or the board browns out the moment you unplug USB.

**Buttons** — left (BOOT) next screen · centre (PWR) cycle brightness, hold 1 s
to reset Wi-Fi · right (GPIO5) force refresh. Touch swipes between screens too.

---

## Setup

### 1. Bridge

Needs Node 18+. No dependencies to install.

```bash
node bridge/bridge.mjs
```

First run writes `bridge/config.json` with a generated token and prints
everything the device needs:

```
  Point the device at:
    host  192.168.1.20
    port  8787
    token <generated-on-first-run>
```

`bridge/config.json` holds that token and is gitignored.

To start it automatically at every logon on Windows:

```bash
powershell -ExecutionPolicy Bypass -File bridge/install-windows-task.ps1
```

`bridge/config.json` options: `port`, `host`, `token`, `limitsEnabled`,
`limitsCacheSec`, `probeModel`, `credentialsPath`, `refreshEnabled`. Env
overrides: `BRIDGE_PORT`, `BRIDGE_TOKEN`, `BRIDGE_LIMITS=0`, `BRIDGE_DEBUG=1`.

The offline parser tests need no credentials and no network:

```bash
node bridge/test-parsers.mjs
```

### 2. Firmware

```bash
pio run -t upload -t monitor
```

PlatformIO pulls LVGL 9, LovyanGFX and ArduinoJson itself. `lv_conf.h` is
deliberately absent — LVGL is configured entirely from `build_flags` via
`LV_CONF_SKIP`, so there is no 1000-line config file to keep in sync.

### 3. Device

On first boot the device has no credentials, so it raises a setup AP:

1. Join **`claude-usage-setup`** (password `claudecode`).
2. The captive portal opens; if it doesn't, browse to `192.168.4.1`.
3. Pick your Wi-Fi, then enter the host / port / token the bridge printed.
4. Save. It reboots and connects.

To reconfigure later, hold the centre button for one second.

---

## Design

Dark only, and strictly on the Anthropic ramp — no traffic-light colours
anywhere. Usage severity is expressed as *intensity* along the warm accents
instead of a hue shift from green to red:

| Role | Hex | |
| --- | --- | --- |
| Background / surface / border | `#191919` `#262625` `#40403E` | |
| Text primary / secondary / muted | `#FAFAF7` `#BFBFBA` `#91918D` | |
| Under 40 % — Kraft | `#D4A27F` | calm |
| 40–75 % — Claude orange | `#D97757` | the primary accent |
| 75–90 % — Book Cloth | `#CC785C` | |
| Over 90 % — Crail | `#C15F3C` | deepest |

The starburst is **rasterised at runtime** in `src/claude_logo.cpp` rather than
shipped as a bitmap: each ray is a kite profile, points are folded into one
canonical sector by symmetry, and 3×3 supersampling gives clean edges. It stays
sharp at any size and the repo carries no copied brand asset.

---

## Layout

```
platformio.ini            build + full LVGL config
include/board_config.h    verified pin map
include/theme.h           Anthropic palette + severity ramp
src/main.cpp              setup/loop, button handling
src/display.cpp           LovyanGFX ST7789 + CST816 wired into LVGL 9
src/ui.cpp                4 tiles, header, footer, splash
src/claude_logo.cpp       procedural starburst rasteriser
src/usage_client.cpp      FreeRTOS poll task, ArduinoJson parse
src/net.cpp               Wi-Fi + captive setup portal
src/board.cpp             power latch, battery ADC, buttons
bridge/bridge.mjs         the whole bridge, zero dependencies
```

---

## Prior art

Built after reading these — worth a look if you want a different board or a
different look:

- [Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) — where the
  rate-limit header approach and the verified 1.54" pin map came from
- [TokenMeter](https://github.com/alestanalves/TokenMeter) — Claude + Codex on a CYD
- [claude-code-usage-monitor](https://github.com/rootedlab-code/claude-code-usage-monitor) — Waveshare 1.47", Python bridge
- [ohmyclawd](https://github.com/opariffazman/ohmyclawd) — pixel-art take
- [ClaudeGauge](https://github.com/dorofino/ClaudeGauge) — LCARS take
- [waveshareteam/ESP32-S3-Touch-LCD-1.54](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.54) — official demos

Not affiliated with Anthropic.
