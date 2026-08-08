#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "app_config.h"
#include "board_config.h"
#include "net.h"
#include "settings.h"

static NetState   s_state = NetState::Boot;
static WebServer  s_http(80);
static DNSServer  s_dns;
static uint32_t   s_connectStartedMs = 0;
static uint32_t   s_lastRetryMs      = 0;
static bool       s_httpUp           = false;

// ------------------------------------------------------------- setup page ----
// Same dark Anthropic palette as the device UI so the two feel like one thing.

static const char PORTAL_CSS[] PROGMEM = R"CSS(
:root{color-scheme:dark}
*{box-sizing:border-box}
body{margin:0;padding:28px 18px;background:#191919;color:#FAFAF7;
     font:15px/1.5 -apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:420px;margin:0 auto}
h1{font-size:20px;margin:0 0 4px;display:flex;align-items:center;gap:10px}
.sub{color:#91918D;font-size:13px;margin:0 0 24px}
.mark{width:22px;height:22px;flex:0 0 22px}
label{display:block;font-size:12px;letter-spacing:.04em;text-transform:uppercase;
      color:#91918D;margin:16px 0 6px}
input,select{width:100%;padding:11px 12px;background:#262625;color:#FAFAF7;
      border:1px solid #40403E;border-radius:8px;font-size:15px}
input:focus,select:focus{outline:none;border-color:#D97757}
button{width:100%;margin-top:24px;padding:13px;background:#D97757;color:#191919;
      border:0;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer}
button:hover{background:#CC785C}
button.danger{background:transparent;color:#C15F3C;border:1px solid #40403E;
      margin-top:12px;font-weight:500}
button.danger:hover{background:#262625;border-color:#C15F3C}
.row{display:flex;gap:12px}.row>div{flex:1}
.hint{color:#666663;font-size:12px;margin-top:6px}
)CSS";

// The starburst again, this time as inline SVG.
static String logo_svg(int size)
{
    String s = "<svg class=\"mark\" viewBox=\"-50 -50 100 100\" width=\"" + String(size) +
               "\" height=\"" + String(size) + "\" fill=\"#D97757\">";
    for (int i = 0; i < 12; i++) {
        s += "<path transform=\"rotate(" + String(i * 30) +
             ")\" d=\"M0 0 L20 -9.5 L50 0 L20 9.5 Z\"/>";
    }
    s += "<circle r=\"5\"/></svg>";
    return s;
}

// Escapes a value before it goes into an HTML attribute. Without this a Wi-Fi
// password containing a quote or an ampersand ends the attribute early: the
// form still renders, but re-saving it silently stores a truncated password
// and the device then fails the WPA handshake forever.
static String esc(const String &in)
{
    String out;
    out.reserve(in.length() + 12);
    for (size_t i = 0; i < in.length(); i++) {
        const char c = in[i];
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
    return out;
}

// Minutes since midnight -> "HH:MM", for <input type=time>.
static String fmt_hhmm(uint16_t minutes)
{
    char buf[6];
    snprintf(buf, sizeof(buf), "%02u:%02u", minutes / 60 % 24, minutes % 60);
    return String(buf);
}

static uint16_t parse_hhmm(const String &v, uint16_t fallback)
{
    const int colon = v.indexOf(':');
    if (colon < 1) return fallback;
    const int h = v.substring(0, colon).toInt();
    const int m = v.substring(colon + 1).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
    return (uint16_t)(h * 60 + m);
}

static void handle_root()
{
    const int n = WiFi.scanComplete();
    String opts;
    for (int i = 0; i < n; i++) {
        const String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        opts += "<option value=\"" + esc(ssid) + "\"" +
                (ssid == g_settings.ssid ? " selected" : "") + ">" + esc(ssid) +
                " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }

    String html;
    html.reserve(4096);
    html += "<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
            "<title>" APP_NAME " setup</title><style>";
    html += FPSTR(PORTAL_CSS);
    html += "</style></head><body><div class=wrap>";
    html += "<h1>" + logo_svg(22) + APP_NAME "</h1>";
    html += "<p class=sub>" BOARD_NAME " &middot; v" APP_VERSION "</p>";
    html += "<form method=POST action=/save>";
    html += "<label>Wi-Fi network</label><select name=ssid>" + opts + "</select>";
    html += "<label>Password</label><input name=pass type=password value=\"" +
            esc(g_settings.pass) + "\">";
    html += "<label>Bridge host</label><input name=host placeholder=\"192.168.1.20\" value=\"" +
            esc(g_settings.bridgeHost) + "\">";
    html += "<p class=hint>IP of the PC running <code>bridge/bridge.mjs</code>.</p>";
    html += "<div class=row><div><label>Port</label><input name=port type=number value=\"" +
            String(g_settings.bridgePort) + "\"></div>";
    html += "<div><label>Poll (s)</label><input name=poll type=number value=\"" +
            String(g_settings.pollMs / 1000) + "\"></div></div>";
    html += "<label>Bridge token</label><input name=token value=\"" +
            esc(g_settings.bridgeToken) + "\">";
    html += "<p class=hint>Printed by the bridge on first start.</p>";

    html += "<label>Spoken alerts</label><select name=alerts>";
    html += String("<option value=1") + (g_settings.alerts ? " selected" : "") + ">On</option>";
    html += String("<option value=0") + (g_settings.alerts ? "" : " selected") + ">Off</option>";
    html += "</select>";
    html += "<p class=hint>Speaks when a limit runs out, and again when it comes back.</p>";

    html += "<div class=row><div><label>Volume</label>"
            "<input name=vol type=number min=0 max=100 value=\"" +
            String(g_settings.volume) + "\"></div>";
    html += "<div><label>Quiet from</label><input name=qs type=time value=\"" +
            fmt_hhmm(g_settings.quietStart) + "\"></div>";
    html += "<div><label>until</label><input name=qe type=time value=\"" +
            fmt_hhmm(g_settings.quietEnd) + "\"></div></div>";
    html += "<p class=hint>Silent inside this range; the screen still updates. "
            "Set both the same to never go quiet. Uses the bridge machine's clock.</p>";

    html += "<button type=submit>Save &amp; reboot</button></form>";

    // Wiping the credentials is a separate, deliberate action — not something
    // that should ride along with an ordinary save.
    html += "<form method=POST action=/forget "
            "onsubmit=\"return confirm('Forget Wi-Fi and restart into setup mode?')\">"
            "<button type=submit class=danger>Forget Wi-Fi</button></form>";
    html += "<p class=hint>Restarts into the <b>" AP_SSID "</b> access point. "
            "Only needed when the network itself changes.</p>";

    html += "</div></body></html>";

    s_http.send(200, "text/html", html);
}

static void handle_forget()
{
    s_http.send(200, "text/html",
                "<meta charset=utf-8><body style='background:#191919;color:#FAFAF7;"
                "font-family:sans-serif;padding:40px;text-align:center'>"
                "<h2 style='color:#D97757'>Wi-Fi forgotten</h2>"
                "<p>Restarting into <b>" AP_SSID "</b>.</p></body>");
    delay(600);
    settings_clear();
    delay(100);
    ESP.restart();
}

static void handle_save()
{
    // Only touch fields the request actually carried. A browser always posts
    // the whole pre-filled form, but a partial POST must not silently wipe the
    // Wi-Fi password just because that field was left out.
    if (s_http.hasArg("ssid"))  g_settings.ssid        = s_http.arg("ssid");
    if (s_http.hasArg("pass"))  g_settings.pass        = s_http.arg("pass");
    if (s_http.hasArg("host"))  g_settings.bridgeHost  = s_http.arg("host");
    if (s_http.hasArg("token")) g_settings.bridgeToken = s_http.arg("token");
    if (s_http.hasArg("port"))  g_settings.bridgePort  = s_http.arg("port").toInt();
    if (s_http.hasArg("poll"))
        g_settings.pollMs = max(5000UL, (unsigned long)s_http.arg("poll").toInt() * 1000UL);
    if (s_http.hasArg("alerts")) g_settings.alerts = s_http.arg("alerts").toInt() != 0;
    if (s_http.hasArg("vol"))
        g_settings.volume = (uint8_t)constrain(s_http.arg("vol").toInt(), 0, 100);
    if (s_http.hasArg("qs")) g_settings.quietStart = parse_hhmm(s_http.arg("qs"), g_settings.quietStart);
    if (s_http.hasArg("qe")) g_settings.quietEnd   = parse_hhmm(s_http.arg("qe"), g_settings.quietEnd);

    if (!g_settings.bridgePort) g_settings.bridgePort = DEFAULT_BRIDGE_PORT;
    settings_save();

    s_http.send(200, "text/html",
                "<meta charset=utf-8><body style='background:#191919;color:#FAFAF7;"
                "font-family:sans-serif;padding:40px;text-align:center'>"
                "<h2 style='color:#D97757'>Saved</h2><p>Rebooting...</p></body>");
    delay(600);
    ESP.restart();
}

// The same form is served in both modes. Once the device is on the LAN it
// stays reachable at its own IP, so fixing a mistyped bridge token does not
// mean wiping the Wi-Fi credentials and starting over.
static void start_http()
{
    if (s_httpUp) return;
    s_http.on("/", handle_root);
    s_http.on("/save", HTTP_POST, handle_save);
    s_http.on("/forget", HTTP_POST, handle_forget);
    s_http.onNotFound(handle_root);  // captive-portal catch-all
    s_http.begin();
    s_httpUp = true;
}

static void start_portal()
{
    s_state = NetState::Portal;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
    WiFi.scanNetworks(true /* async */);

    s_dns.start(53, "*", WiFi.softAPIP());
    start_http();
}

// ----------------------------------------------------------------- public ----

void net_begin()
{
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);

    if (!g_settings.configured()) {
        start_portal();
        return;
    }

    // Length only, never the secret: enough to spot an empty or truncated
    // password, which is otherwise indistinguishable from a wrong one.
    Serial.printf("[net] connecting to \"%s\" (password %u chars)\n",
                  g_settings.ssid.c_str(), (unsigned)g_settings.pass.length());

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(g_settings.ssid.c_str(), g_settings.pass.c_str());
    s_state            = NetState::Connecting;
    s_connectStartedMs = millis();
}

void net_loop()
{
    if (s_state == NetState::Portal) {
        s_dns.processNextRequest();
        s_http.handleClient();
        // Keep the network list fresh while somebody is looking at the form.
        if (WiFi.scanComplete() >= 0 && millis() - s_lastRetryMs > 20000) {
            s_lastRetryMs = millis();
            WiFi.scanDelete();
            WiFi.scanNetworks(true);
        }
        return;
    }

    const bool up = WiFi.status() == WL_CONNECTED;

    if (up) {
        s_state = NetState::Online;
        start_http();           // idempotent; brings the form up on first join
        s_http.handleClient();
        return;
    }

    // A link that drops after being up has to re-arm the retry cycle. Without
    // this the state stays Online, no branch below matches, WiFi.begin() is
    // never reissued, and the device sits there forever showing a network it
    // is no longer on — waiting on an auto-reconnect that has already given up.
    if (s_state == NetState::Online) {
        s_state       = NetState::Failed;
        s_lastRetryMs = millis();
        Serial.println("[net] link lost, retrying");
    }

    if (s_state == NetState::Connecting && millis() - s_connectStartedMs > 20000) {
        s_state       = NetState::Failed;
        s_lastRetryMs = millis();
    } else if (s_state == NetState::Failed && millis() - s_lastRetryMs > 15000) {
        s_lastRetryMs      = millis();
        s_connectStartedMs = millis();
        s_state            = NetState::Connecting;
        WiFi.disconnect();
        WiFi.begin(g_settings.ssid.c_str(), g_settings.pass.c_str());
    }
}

NetState net_state() { return s_state; }
String   net_ip()    { return s_state == NetState::Portal ? WiFi.softAPIP().toString()
                                                          : WiFi.localIP().toString(); }
String   net_ssid()  { return s_state == NetState::Portal ? String(AP_SSID) : WiFi.SSID(); }
int      net_rssi()  { return WiFi.RSSI(); }

void net_start_portal()
{
    settings_clear();
    delay(200);
    ESP.restart();
}
