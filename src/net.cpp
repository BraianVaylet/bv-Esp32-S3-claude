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

static void handle_root()
{
    const int n = WiFi.scanComplete();
    String opts;
    for (int i = 0; i < n; i++) {
        const String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        opts += "<option value=\"" + ssid + "\"" +
                (ssid == g_settings.ssid ? " selected" : "") + ">" + ssid +
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
            g_settings.pass + "\">";
    html += "<label>Bridge host</label><input name=host placeholder=\"192.168.1.20\" value=\"" +
            g_settings.bridgeHost + "\">";
    html += "<p class=hint>IP of the PC running <code>bridge/bridge.mjs</code>.</p>";
    html += "<div class=row><div><label>Port</label><input name=port type=number value=\"" +
            String(g_settings.bridgePort) + "\"></div>";
    html += "<div><label>Poll (s)</label><input name=poll type=number value=\"" +
            String(g_settings.pollMs / 1000) + "\"></div></div>";
    html += "<label>Bridge token</label><input name=token value=\"" +
            g_settings.bridgeToken + "\">";
    html += "<p class=hint>Printed by the bridge on first start.</p>";
    html += "<button type=submit>Save &amp; reboot</button></form></div></body></html>";

    s_http.send(200, "text/html", html);
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
