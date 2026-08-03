#include "M5Dial.h"
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ── Feature gate ─────────────────────────────────────────────────────────
// UDP auto-discovery is written but not yet wired into the boot sequence.
// This is a runtime const, not a #define/#if: an #if guard would strip
// the discovery code from the compiler's view entirely whenever the flag
// is off, which is the same blind spot as commenting it out — bit rot
// would go undetected until the day it's switched on. A plain `if
// (ENABLE_DISCOVERY)` compiles and type-checks the discovery code on
// every build; the optimizer simply removes the dead branch when the
// flag is false, so there's no runtime cost either way. Flip to true
// once Wi-Fi provisioning has been tested on its own.
const bool ENABLE_DISCOVERY = true;

// ── Configuration ────────────────────────────────────────────────────────
// WIFI_SSID / WIFI_PASSWORD are gone — credentials now live in the ESP32's
// own NVS (written either by a previous WiFi.begin(ssid,pass) call or by
// the WiFiManager config portal below) and are reused automatically by
// WiFi.begin() with no arguments.
char LYNX_HOST[64]        = "192.168.1.100";   // Placeholder — set to your
                                                 // own Lynx receiver's IP if
                                                 // not using discovery, or
                                                 // leave as-is and let
                                                 // discovery (ENABLE_DISCOVERY)
                                                 // find it automatically.
                                                 // NOTE: restored for local
                                                 // testing — re-scrub to a
                                                 // placeholder before this
                                                 // firmware is shared publicly.
const uint16_t LYNX_PORT  = 8080;
const unsigned long POLL_INTERVAL_MS = 3000;
const unsigned long MENU_TIMEOUT_MS  = 7000;   // auto-exit browse mode after inactivity

// ── Wi-Fi provisioning config ───────────────────────────────────────────
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 30000;  // saved-creds attempt on boot
const unsigned long WIFI_PORTAL_TIMEOUT_S   = 300;    // 5 min config portal
const char* WIFI_PORTAL_AP_NAME             = "LynxDial-Setup";
const unsigned long WIFI_RETRY_INTERVAL_MS  = 5000;   // runtime watchdog cooldown
                                                         // between reconnect attempts
                                                         // (no portal, no reboot)

// ── Discovery protocol (confirmed design, inert until ENABLE_DISCOVERY) ──
const uint16_t DISCOVERY_PORT           = 9998;
const char*    DISCOVERY_MAGIC          = "LYNX_DISCOVER_V1";
const unsigned long DISCOVERY_TIMEOUT_MS = 1800;   // per-attempt wait for a reply
const int DISCOVERY_MAX_ATTEMPTS         = 3;

// ── App state ────────────────────────────────────────────────────────────
// STATE_WIFI_CONNECTING / STATE_WIFI_SETUP / STATE_DISCOVERING only occur
// during the boot sequence (see setup()). A Wi-Fi drop mid-session never
// enters these states — the runtime watchdog in loop() just retries the
// saved credentials directly and falls back to the existing "WiFi /
// Connect Error" screen in drawScreen() until it reconnects.
enum AppState {
  STATE_NORMAL, STATE_BROWSE, STATE_CONFIRMING,
  STATE_WIFI_CONNECTING, STATE_WIFI_SETUP, STATE_DISCOVERING
};
AppState appState = STATE_NORMAL;

// ── Boot Wi-Fi state ────────────────────────────────────────────────────
unsigned long wifiBootAttemptStart = 0;
unsigned long lastWifiRetryTime    = 0;   // runtime watchdog cooldown timer

// ── Status state (mirrors Lynx's /api/status response) ────────────────────
unsigned long lastPollTime = 0;
// Set to a target millis() timestamp after returning from CONFIRMING to
// STATE_NORMAL; guarantees a second poll within ~1s of a manual tune,
// independent of the normal POLL_INTERVAL_MS cadence. Needed because the
// immediate poll fired the moment CONFIRMING exits can land before Lynx
// has actually finished switching plugs (e.g. tearing down diversity
// mode takes it a moment) — without this, the eye stays in split mode
// until the next regular 3s poll happens to catch the updated state,
// rather than within roughly 1s of the tune actually completing.
unsigned long fastFollowupPollAt = 0;
unsigned long lastSuccessTime = 0;  // last time /api/status actually
                                     // succeeded — drives the 5s offline
                                     // timeout, decoupled from whether
                                     // the displayed VALUES changed
bool   wifiConnected    = false;
bool   statusValid       = false;
String lastError         = "";

String statusMode        = "idle";   // "rf" | "stream" | "idle"
bool   statusOnline      = false;    // Picotuner reachable
bool   statusLocked      = false;
String statusCallsign    = "---";
float  statusFrequency   = 0;        // MHz
float  statusSymbolRate  = 0;        // kS/s
float  statusMer         = 0;
float  statusMargin      = 0;
float  statusLevelDbm    = 0;        // already negated for display
String statusModcod      = "---";
String statusCodec       = "";
String statusAudioCodec  = "";

String statusStreamName    = "";
float  statusBitrateKbps   = -1;     // -1 = no data yet
float  statusDropPercent   = -1;     // -1 = no data yet

// ── Diversity mode (tuner B) — mirrors lynx_overlay.py's fields ──────────
// Populated from doc["diversity"] whenever Lynx reports it enabled.
// Deliberately NOT folded into the statusX fields above (which stay
// tuner-A/"primary" only, same as before) — these are only consulted by
// the split magic-eye and the dual-MER line, so nothing else on screen
// changes behaviour when diversity is off or unavailable.
bool   statusDiversityEnabled = false;
bool   statusLockedB          = false;
float  statusMerB             = 0;
float  statusMarginB          = 0;
float  statusDbmB             = 0;

// Tracks the last state the magic eye was actually drawn for, so we
// only pay the cost of a full eye redraw (fillScreen + arcs + gradient
// rings) when the lock state changes or the signal has moved enough to
// visibly change the wedge angle — everything else (MER wobbling by a
// fraction of a dB) just updates the small text lines below it, which
// only touch their own bounding box and don't cause a visible flash.
bool  eyeLastLocked      = false;
int   eyeLastDbmRounded  = -999;
String eyeLastMode       = "";
// Same idea, tuner B side — only meaningful in diversity mode, but
// harmless to keep populated otherwise since it's simply never
// consulted when statusDiversityEnabled is false.
bool  eyeLastLockedB     = false;
int   eyeLastDbmRoundedB = -999;

// ── Preset memory list — RF and Stream combined, fetched from Lynx ────────
// Rotating the encoder while viewing the normal screen browses this list
// directly — no button press needed to enter browse mode, only to confirm
// a selection. This replaces the earlier button-to-open-menu approach.
enum PresetType { PRESET_RF, PRESET_STREAM };
struct PresetItem {
  PresetType type;
  String label;
  // RF fields
  long   freq;      // kHz
  int    sr;        // kS/s
  String plug;
  long   lnbLoKhz;
  // Stream fields
  String url;
};
#define MAX_ITEMS 30
PresetItem presetItems[MAX_ITEMS];
int    itemCount      = 0;
int    menuSelected   = 0;
long   lastEncoderPos = 0;

// ── Backlight timeout ────────────────────────────────────────────────────
// Backlight-off after inactivity, not true ESP32 deep sleep — deliberate
// choice: M5Stack's own community forum documents that the M5Dial's
// hardware sleep can't be reliably woken by the button when powered via
// USB (only a timer wake works consistently in that mode), and this
// device needs to support USB as one of three fully-working power paths,
// not an edge case. Backlight-off keeps the ESP32 fully awake and
// responsive — wakes instantly on the next input, no boot delay, no
// redraw lag, works identically regardless of which power source is
// active. The backlight is also the dominant power draw on a display
// like this (commonly 60-80%+ of total device power), so this captures
// most of the achievable saving without the USB-wake risk.
const unsigned long BACKLIGHT_TIMEOUT_MS = 5UL * 60UL * 1000UL;  // 5 minutes
const uint8_t BACKLIGHT_ON_LEVEL = 255;  // full brightness — sketch never
                                           // set this explicitly before, so
                                           // this preserves the previous
                                           // always-on-at-default behaviour;
                                           // tune down if a dimmer "on"
                                           // level is preferred.
unsigned long lastInputTime          = 0;
bool          backlightOff           = false;
long          backlightLastEncoderPos = 0;  // separate from lastEncoderPos
                                              // above — this only tracks
                                              // "did the encoder move" for
                                              // wake purposes, independent
                                              // of the browse-menu logic
                                              // that also reads the encoder
unsigned long menuOpenTime = 0;
String confirmingName     = "";
PresetType confirmingType = PRESET_RF;

// ── Forward declarations ────────────────────────────────────────────────
bool runBootWiFiSequence();
void runConfigPortal();
void wifiRuntimeWatchdog();
bool pollLynxStatus();
bool fetchUnifiedPresets();
bool selectItem(int idx);
void drawScreen();
void drawMenu();
void drawConfirming();
void drawWifiConnecting(int secondsLeft);
void drawWifiSetup();
void drawMagicEye(float dbmA, bool activeA, const char* valueTextA,
                   float dbmB, bool activeB, const char* valueTextB,
                   bool split);
bool runLynxDiscovery();
void drawDiscovering();

// -70/-30 range for the Dial specifically — narrower than the OSD
// overlay's -70/-30 (lynx_overlay.py's EYE_DBM_MIN/MAX), by request.
// The two no longer need to match; this just controls how quickly the
// Dial's wedge reaches "fully open" as signal strengthens.
const float PWR_MIN = -70.0f;
const float PWR_MAX = -30.0f;

// Stream-mode wedge range — 0 kb/s (fully closed) to 3000 kb/s (fully
// open), mapped onto the same PWR_MIN..PWR_MAX wedge math the RF eye
// uses via a synthetic dbm-equivalent value (see the streaming block in
// drawScreen()), so both modes share one wedge-angle formula.
const float STREAM_BITRATE_MAX_KBPS = 3000.0f;

// Fixed green used throughout the eye/status display for a locked
// reading — matches lynx_overlay.py's magic-eye ring colour
// (glow_a/glow_b = (0.0, 0.85, 0.25) in Cairo float RGB), used in
// preference to the OSD's separate, brighter text-only green (0x00FF40)
// so the display stays dimmed for longevity. Used as a flat colour
// (not scaled by signal strength) so the reading stays legible at any
// signal level rather than fading dim at the weak end of the scale.
// Picked directly from a colour palette against the real hardware —
// not derived/guessed from RGB math like the earlier attempts.
const uint32_t LOCKED_GREEN = 0x00F45D;

// ── HTTP helpers ─────────────────────────────────────────────────────────
String httpGet(const String& path) {
  HTTPClient http;
  String url = "http://" + String(LYNX_HOST) + ":" + String(LYNX_PORT) + path;
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();
  String payload = "";
  if (code == 200) payload = http.getString();
  http.end();
  return payload;
}

String httpPostJson(const String& path, const String& jsonBody) {
  HTTPClient http;
  String url = "http://" + String(LYNX_HOST) + ":" + String(LYNX_PORT) + path;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  int code = http.POST(jsonBody);
  String payload = "";
  if (code == 200) payload = http.getString();
  http.end();
  return payload;
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, false);
  M5Dial.Display.setRotation(0);
  M5Dial.Display.setTextDatum(middle_center);
  M5Dial.Display.setTextColor(WHITE, BLACK);
  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.fillScreen(BLACK);
  M5Dial.Display.drawString("Build v6.1 - Lynx", M5Dial.Display.width()/2, M5Dial.Display.height()/2);
  delay(1500);
  lastEncoderPos = M5Dial.Encoder.read();
  backlightLastEncoderPos = lastEncoderPos;
  lastInputTime = millis();

  if (ENABLE_DISCOVERY) {
    // Last-known-good IP from a prior successful discovery becomes the
    // starting point / fallback, ahead of the hardcoded default above.
    Preferences prefs;
    prefs.begin("lynxdial", true);
    String saved = prefs.getString("lynx_ip", "");
    prefs.end();
    if (saved.length() > 0) {
      strncpy(LYNX_HOST, saved.c_str(), sizeof(LYNX_HOST) - 1);
      LYNX_HOST[sizeof(LYNX_HOST) - 1] = '\0';
    }
  }

  wifiConnected = runBootWiFiSequence();

  if (ENABLE_DISCOVERY && wifiConnected) {
    appState = STATE_DISCOVERING;
    drawDiscovering();
    runLynxDiscovery();   // updates LYNX_HOST on success; leaves the
                           // hardcoded fallback in place on failure
  }

  appState = STATE_NORMAL;
  lastSuccessTime = millis();
  // Fetch live data immediately on boot rather than waiting for the
  // first periodic poll tick.
  if (wifiConnected) pollLynxStatus();
  drawScreen();
}

void loop() {
  M5Dial.update();

  // ── Backlight timeout ──────────────────────────────────────────────────
  // Checked unconditionally here, before any state-specific branching
  // below, so it applies the same whether the normal screen, browse
  // menu, or confirming screen is showing. wasPressed() is a per-update
  // snapshot flag in M5Unified (not a consumed one-shot event), so
  // reading it here doesn't interfere with the separate BtnA check
  // further down for menu selection — both see the same press.
  {
    long curPos = M5Dial.Encoder.read();
    bool inputNow = (curPos != backlightLastEncoderPos) || M5Dial.BtnA.wasPressed();
    backlightLastEncoderPos = curPos;
    if (inputNow) {
      lastInputTime = millis();
      if (backlightOff) {
        M5Dial.Display.setBrightness(BACKLIGHT_ON_LEVEL);
        backlightOff = false;
      }
    } else if (!backlightOff && millis() - lastInputTime >= BACKLIGHT_TIMEOUT_MS) {
      M5Dial.Display.setBrightness(0);
      backlightOff = true;
    }
  }

  // WiFi watchdog — runtime drops NEVER open the config portal or reboot.
  // Just retry the saved credentials on a cooldown; drawScreen() already
  // shows the "WiFi / Connect Error" screen whenever wifiConnected is
  // false, so no new UI state is needed for this case.
  if (WiFi.status() != WL_CONNECTED) {
    bool justDropped = wifiConnected;
    wifiConnected = false;
    if (justDropped) drawScreen();   // repaint immediately on the transition
    wifiRuntimeWatchdog();
  } else {
    wifiConnected = true;
  }

  // ── NORMAL state ──────────────────────────────────────────────────────
  if (appState == STATE_NORMAL) {
    // Any encoder movement browses the combined RF+Stream list directly —
    // no button press needed to enter browse mode.
    long newPos = M5Dial.Encoder.read();
    long delta  = newPos - lastEncoderPos;
    if (delta != 0 && wifiConnected) {
      if (fetchUnifiedPresets()) {
        appState = STATE_BROWSE;
        menuSelected = ((int)delta % itemCount + itemCount) % itemCount;
        lastEncoderPos = newPos;
        menuOpenTime = millis();
        drawMenu();
        return;
      }
      lastEncoderPos = newPos;  // list fetch failed — don't keep retrying every tick
    }

    // Regular status poll — only redraw if data changed
    unsigned long now = millis();
    if (now - lastPollTime >= POLL_INTERVAL_MS) {
      lastPollTime = now;
      if (wifiConnected) {
        bool changed = pollLynxStatus();
        if (changed) drawScreen();
      }
    }

    // Guaranteed fast re-check after a manual tune (see fastFollowupPollAt
    // declaration) — fires once, independent of the POLL_INTERVAL_MS gate
    // above, so a diversity->single-tuner switch reflects on screen within
    // ~1s rather than waiting for the next regular 3s poll.
    if (fastFollowupPollAt != 0 && now >= fastFollowupPollAt) {
      fastFollowupPollAt = 0;
      if (wifiConnected) {
        bool changed = pollLynxStatus();
        if (changed) drawScreen();
      }
    }

    // Separate 5-second offline check — a poll can fail (single missed
    // request, brief network blip) without immediately flashing to the
    // error screen, but if nothing has succeeded for 5+ seconds Lynx is
    // genuinely offline and the stale last-good reading must not stay
    // frozen on screen indefinitely.
    if (statusValid && (millis() - lastSuccessTime > 5000)) {
      statusValid = false;
      drawScreen();
    }
  }

  // ── BROWSE state ──────────────────────────────────────────────────────
  else if (appState == STATE_BROWSE) {
    bool redraw = false;

    long newPos = M5Dial.Encoder.read();
    long delta  = newPos - lastEncoderPos;
    if (delta != 0) {
      menuSelected = (menuSelected + (int)delta + itemCount) % itemCount;
      lastEncoderPos = newPos;
      menuOpenTime = millis();
      redraw = true;
    }

    // Button press confirms selection
    if (M5Dial.BtnA.wasPressed()) {
      confirmingName = presetItems[menuSelected].label;
      confirmingType = presetItems[menuSelected].type;
      selectItem(menuSelected);
      appState = STATE_CONFIRMING;
      menuOpenTime = millis();
      drawConfirming();
      return;
    }

    // Timeout — cancel and return
    if (millis() - menuOpenTime >= MENU_TIMEOUT_MS) {
      appState = STATE_NORMAL;
      drawScreen();
      return;
    }

    if (redraw) drawMenu();
  }

  // ── CONFIRMING state ────────────────────────────────────────────────
  else if (appState == STATE_CONFIRMING) {
    if (millis() - menuOpenTime >= MENU_TIMEOUT_MS) {
      appState = STATE_NORMAL;
      lastPollTime = 0;  // force immediate poll on return
      fastFollowupPollAt = millis() + 1000;  // and a guaranteed re-check
                                               // ~1s later, in case the
                                               // immediate poll above
                                               // landed before Lynx
                                               // finished switching
      // The eye's own redraw-throttle compares against the LAST value
      // it drew, not against what's actually on screen right now — and
      // drawConfirming() just overwrote the whole display, eye included.
      // Switching between two sources of the SAME type (stream->stream,
      // or RF->RF) can leave eyeLastMode/eyeLastLocked unchanged even
      // though the pixels themselves are gone, so the throttle would
      // wrongly skip the redraw. Resetting eyeLastMode here forces a
      // genuine redraw on the next drawScreen() regardless of whether
      // the underlying value actually moved.
      eyeLastMode = "";
      drawScreen();
    }
  }
}

// ── WiFi ─────────────────────────────────────────────────────────────────
// Boot sequence: try saved credentials (WIFI_CONNECT_TIMEOUT_MS) →
// if that fails, open the config portal (WIFI_PORTAL_TIMEOUT_S) →
// reboot either way once the portal exits, so setup() always runs against
// a known-clean state. Returns true if connected by the time it returns
// (only possible via the saved-creds path — the portal path always ends
// in ESP.restart() and therefore never returns at all).
bool runBootWiFiSequence() {
  WiFi.mode(WIFI_STA);
  WiFi.begin();   // no args — ESP32 reuses the last creds stored in NVS,
                   // whether they got there via a previous plain
                   // WiFi.begin(ssid,pass) or via WiFiManager's portal.

  appState = STATE_WIFI_CONNECTING;
  wifiBootAttemptStart = millis();
  unsigned long lastDraw = 0;

  while (WiFi.status() != WL_CONNECTED &&
         millis() - wifiBootAttemptStart < WIFI_CONNECT_TIMEOUT_MS) {
    M5Dial.update();   // keep the encoder/touch state serviced even
                        // though nothing is actionable yet
    unsigned long now = millis();
    if (now - lastDraw >= 500) {   // repaint the countdown ~2x/sec
      lastDraw = now;
      int secondsLeft = (int)((WIFI_CONNECT_TIMEOUT_MS - (now - wifiBootAttemptStart)) / 1000);
      drawWifiConnecting(secondsLeft);
    }
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) return true;

  // No saved network reachable within the window — open the portal.
  // This never returns; it always ends in ESP.restart().
  runConfigPortal();
  return false;   // unreachable, kept for compiler clarity
}

// Blocking captive-portal config screen. Reachable ONLY from the boot
// sequence above — never from the runtime watchdog — per the earlier
// design decision that a mid-session drop should retry quietly rather
// than interrupt whatever's on screen with a portal.
//
// Deliberately does NOT branch on save-vs-timeout: WiFiManager does not
// clear previously saved NVS credentials on a bare portal timeout, so
// "nobody showed up to reconfigure" and "somebody entered new creds" both
// resolve the same way — restart, and let the next boot's saved-creds
// attempt sort out which one actually happened. This is what gives the
// self-healing behavior for out-of-range/outage cases: an unattended
// Dial keeps cycling connect-attempt → portal → restart until the
// original network comes back, rather than getting stuck open in AP mode.
void runConfigPortal() {
  appState = STATE_WIFI_SETUP;
  drawWifiSetup();

  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_PORTAL_TIMEOUT_S);
  // Override WiFiManager's default blue/red theme — a <style> element here
  // overwrites their built-in CSS rather than merging with it, so this
  // needs to cover body background/text and the button/link colors too,
  // not just the top-level page.
  wm.setCustomHeadElement(
    "<style>"
    "body{background:#000!important;color:#fff!important;}"
    "h1,h2,h3,p,label,div{color:#fff!important;}"
    "input,select{background:#111!important;color:#fff!important;"
    "border:1px solid #444!important;}"
    // The scanned-network list is rendered as plain <a> links (not
    // .btn), so it needs its own rule — without this, those entries
    // likely rendered as dark text on the forced black body background
    // (invisible, not literally absent), which is probably why the
    // list appeared empty. Buttons get an additional background/border
    // treatment on top of the same base link color.
    "a{color:#fff!important;}"
    "button,.btn,a.btn{background:#222!important;color:#fff!important;"
    "border:1px solid #666!important;}"
    "</style>"
  );
  wm.startConfigPortal(WIFI_PORTAL_AP_NAME);   // blocks up to the timeout;
                                                 // on save, connects AND
                                                 // persists creds to NVS
                                                 // before returning.
  ESP.restart();
}

// Runtime watchdog — called every loop() tick while disconnected.
// Intentionally lightweight: no portal, no reboot, just a rate-limited
// retry against whatever's already saved in NVS.
void wifiRuntimeWatchdog() {
  unsigned long now = millis();
  if (now - lastWifiRetryTime < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetryTime = now;
  WiFi.reconnect();
}

// ── Lynx discovery client ───────────────────────────────────────────────
// Broadcasts DISCOVERY_MAGIC on DISCOVERY_PORT, waits for a unicast JSON
// reply of the form {"protocol_version":1,"name":...,"callsign":...,
// "ip":...,"api_port":...}, up to DISCOVERY_MAX_ATTEMPTS times.
//
// NOTE — not yet exercised (ENABLE_DISCOVERY is false): this compiles and
// is checked on every build, but until the flag flips it never runs, and
// there's no Lynx-side responder listening on :9998 yet either. Only
// handles the first reply received; if multiple Lynx instances answer on
// one LAN, this silently keeps whichever reply arrives first. A
// name/callsign picker screen for that case is a known follow-on, not
// yet built.
//
// On success: overwrites LYNX_HOST and persists it to NVS.
// On failure (no reply after all attempts): leaves LYNX_HOST untouched,
// so the hardcoded/last-known-good value from setup() keeps being used.
bool runLynxDiscovery() {
  WiFiUDP udp;
  udp.begin(DISCOVERY_PORT);

  for (int attempt = 0; attempt < DISCOVERY_MAX_ATTEMPTS; attempt++) {
    udp.beginPacket(IPAddress(255, 255, 255, 255), DISCOVERY_PORT);
    udp.write((const uint8_t*)DISCOVERY_MAGIC, strlen(DISCOVERY_MAGIC));
    udp.endPacket();

    unsigned long waitStart = millis();
    while (millis() - waitStart < DISCOVERY_TIMEOUT_MS) {
      int packetSize = udp.parsePacket();
      if (packetSize > 0) {
        char buf[256];
        int len = udp.read(buf, sizeof(buf) - 1);
        if (len > 0) {
          buf[len] = '\0';
          DynamicJsonDocument doc(256);
          if (!deserializeJson(doc, buf)) {
            const char* ip = doc["ip"] | "";
            if (strlen(ip) > 0) {
              strncpy(LYNX_HOST, ip, sizeof(LYNX_HOST) - 1);
              LYNX_HOST[sizeof(LYNX_HOST) - 1] = '\0';

              Preferences prefs;
              prefs.begin("lynxdial", false);
              prefs.putString("lynx_ip", LYNX_HOST);
              prefs.end();

              udp.stop();
              return true;
            }
          }
        }
      }
      delay(50);
    }
    // No reply this attempt — short backoff before retrying, per the
    // confirmed design (avoids hammering the network on a dropped packet).
    delay(300);
  }

  udp.stop();
  return false;
}

// ── Lynx status query ──────────────────────────────────────────────────
// Lynx's /api/status nests everything under "lynx" and "picotuner", and
// numeric readings (mer, level, symbol_rate etc) come through as STRINGS
// rather than JSON numbers — the Picotuner's own status broadcast is
// text-based, and Lynx passes several fields through largely unmodified.
bool pollLynxStatus() {
  String response = httpGet("/api/status");
  if (response.length() == 0) {
    lastError = "No response";
    return false;   // don't flip statusValid here — a single missed poll
                     // shouldn't flash to the error screen; the 5s
                     // timeout in loop() decides when it's genuinely
                     // offline rather than a one-off blip.
  }

  // Sized generously after a real-world failure: diversity mode plus an
  // active stream produces a raw JSON response of ~1650 bytes (measured
  // directly from a live Lynx receiver), and a parsed ArduinoJson
  // document needs meaningfully more than the raw string length to hold
  // it — the previous 1024-byte allocation was overflowing on exactly
  // this combination, which deserializeJson() reports as a parse
  // failure ("JSON error" below) even though the JSON itself was
  // perfectly valid. ESP32 has ample RAM for a few KB here, so this
  // errs well on the generous side rather than trying to trim it to
  // the minimum that happens to work today.
  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, response)) {
    lastError = "JSON error";
    return false;
  }

  JsonObject lynx = doc["lynx"];
  JsonObject pt   = doc["picotuner"];
  JsonObject div  = doc["diversity"];
  bool divEnabled = div["enabled"] | false;
  JsonObject tunerB = divEnabled ? div["tuner_b"].as<JsonObject>() : JsonObject();

  bool   prevLocked   = statusLocked;
  String prevCallsign = statusCallsign;
  float  prevFreq     = statusFrequency;
  float  prevMer      = statusMer;
  float  prevLevel    = statusLevelDbm;
  String prevMode     = statusMode;
  String prevStream   = statusStreamName;
  float  prevDrop     = statusDropPercent;
  float  prevBitrate  = statusBitrateKbps;

  statusMode       = lynx["mode"] | "idle";
  statusStreamName = lynx["stream_name"] | "";
  // Confirmed against lynx_app.py directly: bitrate is nested under
  // stream_info.bitrate_kbps, not a top-level "average_bitrate_kbps" —
  // that field never existed, which is why this always showed "--".
  JsonObject streamInfo = lynx["stream_info"];
  JsonVariant br = streamInfo["bitrate_kbps"];
  statusBitrateKbps = br.isNull() ? -1 : br.as<float>();
  // NOTE: unlike bitrate above, "stream_drop_percent" isn't just the
  // wrong field name — Lynx doesn't currently expose any drop/loss
  // metric for streams at all (confirmed against lynx_app.py). This
  // will keep reading -1 ("--" on screen) until Lynx adds one; nothing
  // to fix on the Dial side until that metric exists server-side.
  JsonVariant drop  = lynx["stream_drop_percent"];
  statusDropPercent = drop.isNull() ? -1 : drop.as<float>();

  statusOnline     = pt["online"] | false;
  statusLocked     = pt["locked"] | false;
  statusCallsign   = pt["callsign"].as<String>();
  if (statusCallsign.length() == 0) statusCallsign = "---";

  String freqStr   = pt["frequency"].as<String>();
  statusFrequency  = freqStr.length() ? freqStr.toFloat() : 0;
  String srStr     = pt["symbol_rate"].as<String>();
  statusSymbolRate = srStr.length() ? srStr.toFloat() : 0;
  String merStr    = pt["mer"].as<String>();
  statusMer        = merStr.length() ? merStr.toFloat() : 0;
  String marginStr = pt["margin"].as<String>();
  statusMargin     = marginStr.length() ? marginStr.toFloat() : 0;

  // Prefer the real dBm reading (ptwh0v3k+'s look-up table, same field
  // the overlay now uses) — falls back to the older negated-"level"
  // approximation only when talking to Picotuner firmware that doesn't
  // send "dbm" yet.
  String dbmStr    = pt["dbm"].as<String>();
  if (dbmStr.length()) {
    statusLevelDbm = dbmStr.toFloat();
  } else {
    String levelStr = pt["level"].as<String>();
    statusLevelDbm  = levelStr.length() ? -levelStr.toFloat() : 0;
  }

  statusModcod     = pt["modcod"].as<String>();
  if (statusModcod.length() == 0) statusModcod = "---";
  statusCodec      = pt["codec"].as<String>();
  statusAudioCodec = pt["audio_codec"].as<String>();

  // ── Diversity mode (tuner B) ─────────────────────────────────────────
  statusDiversityEnabled = divEnabled;
  if (divEnabled) {
    statusLockedB = tunerB["locked"] | false;
    String merBStr    = tunerB["mer"].as<String>();
    statusMerB         = merBStr.length() ? merBStr.toFloat() : 0;
    String marginBStr = tunerB["margin"].as<String>();
    statusMarginB      = marginBStr.length() ? marginBStr.toFloat() : 0;
    String dbmBStr    = tunerB["dbm"].as<String>();
    if (dbmBStr.length()) {
      statusDbmB = dbmBStr.toFloat();
    } else {
      String levelBStr = tunerB["level"].as<String>();
      statusDbmB        = levelBStr.length() ? -levelBStr.toFloat() : 0;
    }
  } else {
    statusLockedB = false;
    statusMerB = statusMarginB = statusDbmB = 0;
  }

  bool wasInvalid = !statusValid;
  statusValid = true; lastError = "";
  lastSuccessTime = millis();

  return (wasInvalid || statusLocked != prevLocked || statusCallsign != prevCallsign ||
          statusMode != prevMode || statusStreamName != prevStream ||
          fabs(statusFrequency - prevFreq) > 0.001f ||
          (statusLocked && fabs(statusMer - prevMer) >= 0.5f) ||
          (statusLocked && fabs(statusLevelDbm - prevLevel) >= 1.0f) ||
          (statusMode == "stream" && fabs(statusDropPercent - prevDrop) >= 0.1f) ||
          (statusMode == "stream" && fabs(statusBitrateKbps - prevBitrate) >= 5.0f));
}

// ── Fetch combined RF + Stream preset list from Lynx ──────────────────
bool fetchUnifiedPresets() {
  itemCount = 0;

  // RF presets — GET /api/presets returns {"local": [...], "ryde": [...]}
  // Saved "stream memories" now live in this SAME list, distinguished by
  // a "type" field (per the updated lynx_config.yaml) — this previously
  // assumed every entry here was RF and always populated freq/sr/plug
  // regardless, which is exactly why stream memories showed up but
  // couldn't be selected: they had no real freq/sr, so selecting one
  // sent a meaningless /api/tune call instead of /api/stream.
  String rfResp = httpGet("/api/presets");
  DynamicJsonDocument rfDoc(4096);
  if (!deserializeJson(rfDoc, rfResp)) {
    JsonArray local = rfDoc["local"].as<JsonArray>();
    for (JsonObject p : local) {
      if (itemCount >= MAX_ITEMS) break;
      String raw = p["name"] | "?";
      raw.replace("_", " ");
      String ptype = p["type"] | "rf";   // absent "type" = older RF-only
                                           // config, defaults to RF for
                                           // backward compatibility
      if (ptype == "stream") {
        presetItems[itemCount].type  = PRESET_STREAM;
        presetItems[itemCount].label = raw;
        // ASSUMPTION: stream memories store their URL under "url",
        // matching the old top-level /api/streams entries' field name.
        // Not yet confirmed against Lynx's actual save-stream-memory
        // response — if selecting one still fails after this, check
        // the real field name in Lynx's /api/presets output and it's
        // likely just this key that needs changing.
        presetItems[itemCount].url   = String((const char*)(p["url"] | ""));
      } else {
        presetItems[itemCount].type     = PRESET_RF;
        presetItems[itemCount].label    = raw;
        presetItems[itemCount].freq     = p["freq"] | 0;
        presetItems[itemCount].sr       = p["sr"] | 0;
        presetItems[itemCount].plug     = String((const char*)(p["plug"] | "a"));
        presetItems[itemCount].lnbLoKhz = p["lnb_lo_khz"] | 0;
      }
      itemCount++;
    }
  }

  // Stream presets — GET /api/streams returns {"streams": [...]}
  String stResp = httpGet("/api/streams");
  DynamicJsonDocument stDoc(4096);
  if (!deserializeJson(stDoc, stResp)) {
    JsonArray streams = stDoc["streams"].as<JsonArray>();
    for (JsonObject s : streams) {
      if (itemCount >= MAX_ITEMS) break;
      presetItems[itemCount].type  = PRESET_STREAM;
      presetItems[itemCount].label = String((const char*)(s["name"] | "?"));
      presetItems[itemCount].url   = String((const char*)(s["url"] | ""));
      itemCount++;
    }
  }

  return itemCount > 0;
}

// ── Select a preset by index — Lynx does all the work ─────────────────
bool selectItem(int idx) {
  PresetItem &it = presetItems[idx];
  if (it.type == PRESET_RF) {
    String body = "{\"freq\":" + String(it.freq) +
                  ",\"sr\":" + String(it.sr) +
                  ",\"plug\":\"" + it.plug + "\"" +
                  ",\"lnb_lo_khz\":" + String(it.lnbLoKhz) + "}";
    httpPostJson("/api/tune", body);
  } else {
    String body = "{\"url\":\"" + it.url + "\",\"name\":\"" + it.label + "\"}";
    httpPostJson("/api/stream", body);
  }
  return true;
}

// ── Draw scrolling combined preset menu ────────────────────────────────
void drawMenu() {
  int cx = M5Dial.Display.width() / 2;
  int cy = M5Dial.Display.height() / 2;

  M5Dial.Display.fillScreen(BLACK);

  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.setTextColor(0xFFAA00, BLACK);
  M5Dial.Display.drawString("MEMORIES", cx, 22);

  const int itemH = 36;

  for (int i = -2; i <= 2; i++) {
    int idx = ((menuSelected + i) % itemCount + itemCount) % itemCount;
    int y   = cy + (i * itemH);
    bool isStream = (presetItems[idx].type == PRESET_STREAM);

    if (i == 0) {
      // Selected item — white outline box, white text
      M5Dial.Display.drawRoundRect(cx - 90, y - 14, 180, 28, 6, WHITE);
      M5Dial.Display.setTextFont(&fonts::Font2);
      M5Dial.Display.setTextColor(WHITE, BLACK);
      M5Dial.Display.drawString(presetItems[idx].label, cx, y);
      // Small type tag beneath the box — RF (blue) or STREAM (purple)
      M5Dial.Display.setTextFont(&fonts::Font0);
      M5Dial.Display.setTextColor(isStream ? 0xB388FF : 0x40A0FF, BLACK);
      M5Dial.Display.drawString(isStream ? "STREAM" : "RF", cx, y + 20);
    } else if (abs(i) == 1) {
      M5Dial.Display.setTextFont(&fonts::Font2);
      M5Dial.Display.setTextColor(0xFFDD00, BLACK);
      M5Dial.Display.drawString(presetItems[idx].label, cx, y);
    } else {
      M5Dial.Display.setTextFont(&fonts::Font2);
      M5Dial.Display.setTextColor(0x444444, BLACK);
      M5Dial.Display.drawString(presetItems[idx].label, cx, y);
    }
  }

  // Timeout dots
  unsigned long elapsed = millis() - menuOpenTime;
  int dotsLeft = 6 - (int)(elapsed * 6 / MENU_TIMEOUT_MS);
  for (int d = 0; d < 6; d++) {
    M5Dial.Display.fillCircle(cx - 30 + d * 12, 212, 4,
      (d < dotsLeft) ? 0x666666 : 0x1A1A1A);
  }
}

// ── Draw confirmation screen ────────────────────────────────────────────
void drawConfirming() {
  int cx = M5Dial.Display.width() / 2;
  int cy = M5Dial.Display.height() / 2;
  // Font2's line height is ~20px; 1.5x that ≈ 30px. Both the name and
  // "please wait..." shift down together so the gap between them stays
  // the same as before — only their position relative to the "Tuning
  // to"/"Playing" title above changes.
  const int NAME_Y_SHIFT = 30;
  M5Dial.Display.fillScreen(0x0A1A0A);
  M5Dial.Display.setTextColor(0x00FF40, 0x0A1A0A);
  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.drawString(confirmingType == PRESET_STREAM ? "Playing" : "Tuning to",
                             cx, cy - 30);
  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.setTextColor(WHITE, 0x0A1A0A);
  M5Dial.Display.drawString(confirmingName, cx, cy + 5 + NAME_Y_SHIFT);
  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.setTextColor(0x448844, 0x0A1A0A);
  M5Dial.Display.drawString("please wait...", cx, cy + 35 + NAME_Y_SHIFT);
}

// ── Boot Wi-Fi screens ──────────────────────────────────────────────────
// Same draining-dot-row visual language as the MENU_TIMEOUT_MS countdown
// in drawMenu(), reused here so a 30s wait reads as "actively working"
// rather than a hang.
void drawWifiConnecting(int secondsLeft) {
  int cx = M5Dial.Display.width() / 2;
  int cy = M5Dial.Display.height() / 2;
  M5Dial.Display.fillScreen(BLACK);
  M5Dial.Display.setTextFont(&fonts::Font4);
  M5Dial.Display.setTextColor(WHITE, BLACK);
  M5Dial.Display.drawString("Connecting", cx, cy - 30);
  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.setTextColor(0x888888, BLACK);
  M5Dial.Display.drawString("to saved Wi-Fi...", cx, cy);

  int dotsLeft = (secondsLeft * 6) / (int)(WIFI_CONNECT_TIMEOUT_MS / 1000);
  dotsLeft = constrain(dotsLeft, 0, 6);
  for (int d = 0; d < 6; d++) {
    M5Dial.Display.fillCircle(cx - 30 + d * 12, cy + 40, 4,
      (d < dotsLeft) ? 0x40A0FF : 0x1A1A1A);
  }
}

void drawWifiSetup() {
  int cx = M5Dial.Display.width() / 2;
  int cy = M5Dial.Display.height() / 2;
  M5Dial.Display.fillScreen(0x140A00);
  M5Dial.Display.setTextFont(&fonts::Font4);
  M5Dial.Display.setTextColor(0xFFAA00, 0x140A00);
  M5Dial.Display.drawString("Wi-Fi Setup", cx, cy - 50);
  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.setTextColor(WHITE, 0x140A00);
  M5Dial.Display.drawString("Connect a phone/laptop to:", cx, cy - 10);
  M5Dial.Display.setTextColor(0x40A0FF, 0x140A00);
  M5Dial.Display.drawString(WIFI_PORTAL_AP_NAME, cx, cy + 16);
  M5Dial.Display.setTextColor(0x888888, 0x140A00);
  M5Dial.Display.drawString("then follow the prompt", cx, cy + 46);
  M5Dial.Display.drawString("to enter your password", cx, cy + 68);
}

void drawDiscovering() {
  int cx = M5Dial.Display.width() / 2;
  int cy = M5Dial.Display.height() / 2;
  M5Dial.Display.fillScreen(BLACK);
  M5Dial.Display.setTextFont(&fonts::Font4);
  M5Dial.Display.setTextColor(WHITE, BLACK);
  M5Dial.Display.drawString("Finding Lynx...", cx, cy);
}

// ── Magic Eye ────────────────────────────────────────────────────────────
// Classic valve-radio "magic eye" style signal indicator, matching the
// Lynx web overlay's look — one full-screen eye, split top/bottom in
// diversity mode (tuner A above the centre line, tuner B below), same as
// the OSD overlay's design.
//
// APPROACH (second rewrite — see git history): the first attempt tried to
// carve one ring into independently-shadowed halves using angle
// arithmetic, which required guessing M5GFX's angle-zero direction —
// guessed wrong twice in a row. This version draws the SAME full-size
// single-eye geometry twice, unmodified, at the SAME centre point and
// SAME radius as single-tuner mode — and uses setClipRect() to restrict
// each draw to its own half of the screen by literal pixel row, not by
// angle. A pixel clip is unambiguous regardless of any angle convention,
// so there is nothing left to get backwards: whichever rows are inside
// the clip rectangle are exactly what appears, full stop. The two draws
// together read as one large eye, split top/bottom, at full screen size —
// not two small separate eyes.
void drawSingleEye(int cx, int cy, int outerR, int innerR,
                    float dbm, bool active, const char* valueText,
                    int textYOffset) {
  float pwr = constrain(dbm, PWR_MIN, PWR_MAX);
  float t = (pwr - PWR_MIN) / (PWR_MAX - PWR_MIN);
  // t is used ONLY here, for wedge size — signal strength should not
  // affect brightness/colour anywhere (that was the actual bug behind
  // the top/bottom hole-brightness mismatch: colour was scaling with t
  // as well as the wedge, so two different-but-both-locked readings
  // produced two different brightnesses even in the hole gradient).
  float halfAngleDeg = 80.0f * (1.0f - t) + 27.0f * t;

  uint8_t glowR, glowG, glowB;
  uint32_t shadowColour;
  if (active) {
    // Fixed bright green, matching LOCKED_GREEN — no longer scaled by t.
    // Ring colour is now independent from LOCKED_GREEN (the text colour)
    // — a separate palette pick, per request, while the text stays on
    // its own value.
    glowR = 0x22; glowG = 0x8E; glowB = 0x38;
    shadowColour = M5Dial.Display.color888(0x05, 0x1A, 0x05);
  } else {
    // Fixed red, matching the "NO LOCK" status accent colour (0xFF4040)
    // used elsewhere on screen — no longer scaled by t.
    glowR = 0xFF; glowG = 0x40; glowB = 0x40;
    shadowColour = M5Dial.Display.color888(0x1A, 0x05, 0x05);
  }
  uint32_t glowColour = M5Dial.Display.color888(glowR, glowG, glowB);

  M5Dial.Display.fillArc(cx, cy, innerR, outerR, 0, 360, glowColour);
  M5Dial.Display.fillArc(cx, cy, innerR, outerR,
                          180 - halfAngleDeg, 180 + halfAngleDeg, shadowColour);
  M5Dial.Display.fillArc(cx, cy, innerR, outerR,
                          -halfAngleDeg, halfAngleDeg, shadowColour);

  const int GLOW_STEPS = 8;
  for (int i = GLOW_STEPS; i >= 0; i--) {
    float frac = (float)i / GLOW_STEPS;
    int rr = (int)(innerR * frac);
    uint8_t rC = (uint8_t)(glowR * frac * 0.5f);
    uint8_t gC = (uint8_t)(glowG * frac * 0.5f);
    uint8_t bC = (uint8_t)(glowB * frac * 0.5f);
    M5Dial.Display.fillCircle(cx, cy, rr, M5Dial.Display.color888(rC, gC, bC));
  }

  if (valueText != nullptr && strlen(valueText) > 0) {
    M5Dial.Display.setTextFont(&fonts::Font2);
    // Fixed bright LOCKED_GREEN for a locked reading, matching the
    // "LOCKED" status text, rather than the ring's own signal-graded
    // glow colour — keeps the digits legible at any signal strength
    // instead of fading dim at the weak end of the scale.
    // Transparent background — single-argument setTextColor draws the
    // glyphs directly over whatever's already there (the gradient/ring)
    // rather than stamping an opaque black rectangle behind the text.
    // Contrast is already good without it.
    M5Dial.Display.setTextColor(active ? LOCKED_GREEN : glowColour);
    M5Dial.Display.drawString(valueText, cx, cy + textYOffset);
  }
}

// dbmA/dbmB: true dBm readings (see pollLynxStatus()), same -70..-30
//            scale as the overlay. activeA/activeB: true = green glow
//            (locked), false = red glow (searching). valueTextA/B: shown
//            near the centre, offset up/down in split mode to stay clear
//            of the clip boundary. split: false outside diversity mode —
//            draws a single full-size unified eye exactly as before,
//            ignoring the B parameters entirely.
void drawMagicEye(float dbmA, bool activeA, const char* valueTextA,
                   float dbmB, bool activeB, const char* valueTextB,
                   bool split) {
  int w = M5Dial.Display.width();
  int h = M5Dial.Display.height();
  int cx = w / 2;
  int cy = h / 2;
  int outerR = w / 2;
  // Hole 10% larger than before the dBm/split work (was outerR*0.42).
  // Text size unchanged.
  int innerR = (int)(outerR * 0.462f);

  M5Dial.Display.fillScreen(BLACK);

  if (!split) {
    drawSingleEye(cx, cy, outerR, innerR, dbmA, activeA, valueTextA, 0);
    return;
  }

  // Diversity mode — same full-size eye geometry as above, drawn twice
  // at the same centre/radius, each confined to its own half of the
  // screen by a pixel clip rather than by angle. Text nudged 14px up
  // (A) / down (B) from centre so it lands clearly inside its own half
  // rather than straddling the clip boundary at cy.
  M5Dial.Display.setClipRect(0, 0, w, h / 2);
  drawSingleEye(cx, cy, outerR, innerR, dbmA, activeA, valueTextA, -14);
  M5Dial.Display.clearClipRect();

  M5Dial.Display.setClipRect(0, h / 2, w, h - h / 2);
  drawSingleEye(cx, cy, outerR, innerR, dbmB, activeB, valueTextB, 14);
  M5Dial.Display.clearClipRect();
}

// ── Display ─────────────────────────────────────────────────────────────
void drawScreen() {
  int cx = M5Dial.Display.width() / 2;
  int cy = M5Dial.Display.height() / 2;

  if (!wifiConnected) {
    M5Dial.Display.fillScreen(BLACK);
    M5Dial.Display.setTextColor(WHITE, BLACK);
    M5Dial.Display.setTextFont(&fonts::Font4);
    M5Dial.Display.drawString("WiFi", cx, cy - 20);
    M5Dial.Display.drawString("Connect Error", cx, cy + 20);
    return;
  }

  if (!statusValid) {
    uint32_t offlineColour = 0x10141C;
    M5Dial.Display.fillScreen(offlineColour);
    M5Dial.Display.setTextColor(0xFFAA00, offlineColour);
    M5Dial.Display.setTextFont(&fonts::Font4);
    M5Dial.Display.drawString("Lynx", cx, cy - 20);
    M5Dial.Display.drawString("Connect Error", cx, cy + 20);
    M5Dial.Display.setTextColor(0x888888, offlineColour);
    M5Dial.Display.setTextFont(&fonts::Font2);
    M5Dial.Display.drawString(lastError, cx, cy + 60);
    return;
  }

  // ── Streaming ────────────────────────────────────────────────────────
  if (statusMode == "stream") {
    // Centre of the eye now shows kb/s (was drop%) — drop% moves down to
    // the lower line where kb/s used to be, so neither figure is lost,
    // just swapped positions per request.
    char kbpsLine[24];
    if (statusBitrateKbps >= 0) {
      snprintf(kbpsLine, sizeof(kbpsLine), "%.0f kb/s", statusBitrateKbps);
    } else {
      strcpy(kbpsLine, "--");
    }
    // Same flash-avoidance approach as RF mode — only redraw the eye
    // when it's newly active or the bitrate has moved enough to matter,
    // not on every small wobble. eyeLastMode != "stream" also catches
    // genuinely switching INTO stream mode from RF — but NOT switching
    // from one stream to another, since mode stays "stream" throughout;
    // that case is handled separately below.
    int bitrateRounded = (statusBitrateKbps >= 0) ? (int)statusBitrateKbps : -999;
    bool eyeNeedsRedraw = (eyeLastMode != "stream") || (!eyeLastLocked) ||
                          (abs(bitrateRounded - eyeLastDbmRounded) >= 5);
    if (eyeNeedsRedraw) {
      // Wedge angle now genuinely tracks bitrate — 0 kb/s maps to
      // PWR_MIN (fully closed/weak-looking), STREAM_BITRATE_MAX_KBPS
      // (3000) and above maps to PWR_MAX (fully open/strong-looking),
      // via the same synthetic-dbm trick used for the wedge math
      // regardless of mode. Previously this was hardcoded to PWR_MAX
      // always, so the wedge never actually moved for streams.
      float bitrateFrac = (statusBitrateKbps >= 0)
        ? constrain(statusBitrateKbps / STREAM_BITRATE_MAX_KBPS, 0.0f, 1.0f)
        : 0.0f;
      float synthDbm = PWR_MIN + bitrateFrac * (PWR_MAX - PWR_MIN);
      drawMagicEye(synthDbm, true, kbpsLine, 0, false, "", false);
      eyeLastLocked = true;
      eyeLastDbmRounded = bitrateRounded;
      eyeLastMode = "stream";
    }

    M5Dial.Display.setTextFont(&fonts::Font2);
    M5Dial.Display.setTextColor(0x00FF40);
    M5Dial.Display.drawString("STREAMING", cx, 28);
    M5Dial.Display.setTextFont(&fonts::Font2);
    M5Dial.Display.setTextColor(WHITE);
    M5Dial.Display.drawString(statusStreamName.length() ? statusStreamName : "Stream", cx, cy + 90);
    if (statusDropPercent >= 0) {
      char dropLine[24];
      snprintf(dropLine, sizeof(dropLine), "%.1f%% drop", statusDropPercent);
      // Not a full matching 30px shift — the round screen's visible width
      // narrows quickly this close to the bottom edge, so this stays a
      // bit more conservative to avoid the text clipping against the
      // physical bezel.
      M5Dial.Display.drawString(dropLine, cx, cy + 108);
    }
    return;
  }

  // ── RF (DVB-S/S2 via Picotuner) — magic eye ───────────────────────────
  char dbmLine[24];
  if (statusLocked) {
    snprintf(dbmLine, sizeof(dbmLine), "%.0f dBm", statusLevelDbm);
  } else {
    strcpy(dbmLine, "");
  }
  char dbmLineB[24];
  if (statusDiversityEnabled && statusLockedB) {
    snprintf(dbmLineB, sizeof(dbmLineB), "%.0f dBm", statusDbmB);
  } else {
    strcpy(dbmLineB, "");
  }

  int dbmRounded  = (int)(statusLocked ? statusLevelDbm : PWR_MIN);
  int dbmRoundedB = (int)((statusDiversityEnabled && statusLockedB) ? statusDbmB : PWR_MIN);
  bool eyeNeedsRedraw = (eyeLastMode != "rf") ||
                        (statusLocked != eyeLastLocked) ||
                        (abs(dbmRounded - eyeLastDbmRounded) >= 2) ||
                        (statusDiversityEnabled &&
                         ((statusLockedB != eyeLastLockedB) ||
                          (abs(dbmRoundedB - eyeLastDbmRoundedB) >= 2)));
  if (eyeNeedsRedraw) {
    drawMagicEye(statusLocked ? statusLevelDbm : PWR_MIN, statusLocked, dbmLine,
                 statusDiversityEnabled ? (statusLockedB ? statusDbmB : PWR_MIN) : 0,
                 statusDiversityEnabled && statusLockedB, dbmLineB,
                 statusDiversityEnabled);
    eyeLastLocked      = statusLocked;
    eyeLastDbmRounded   = dbmRounded;
    eyeLastLockedB      = statusLockedB;
    eyeLastDbmRoundedB  = dbmRoundedB;
    eyeLastMode = "rf";
  }

  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.setTextColor(WHITE);
  M5Dial.Display.drawString(statusLocked ? "LOCKED" : "NO LOCK", cx, 28);

  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.setTextColor(WHITE);
  M5Dial.Display.drawString(statusLocked ? statusCallsign : "Unlocked", cx, cy + 106);

  char freqLine[40];
  snprintf(freqLine, sizeof(freqLine), "%.3f MHz", statusFrequency);
  M5Dial.Display.setTextFont(&fonts::Font2);
  M5Dial.Display.drawString(freqLine, cx, cy + 84);

  char srMerLine[60];
  if (statusDiversityEnabled) {
    // "MER 19/21 dB" — A/B combined, whole dB per spec.
    snprintf(srMerLine, sizeof(srMerLine), "%.0f kS  MER %.0f/%.0f dB",
             statusSymbolRate, statusMer, statusMerB);
  } else {
    snprintf(srMerLine, sizeof(srMerLine), "%.0f kS  MER %.1f dB",
             statusSymbolRate, statusMer);
  }
  M5Dial.Display.drawString(srMerLine, cx, cy + 60);
}
