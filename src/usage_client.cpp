#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "app_config.h"
#include "net.h"
#include "settings.h"
#include "usage_client.h"

static UsageSnapshot     s_snap;
static SemaphoreHandle_t s_lock  = nullptr;
static TaskHandle_t      s_task  = nullptr;

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void read_window(JsonObjectConst obj, Window &w)
{
    if (obj.isNull()) { w.valid = false; return; }
    w.pct        = obj["pct"] | 0.0f;
    w.resetInSec = obj["resetInSec"] | 0;
    w.valid      = true;
}

static bool parse_payload(const String &body, UsageSnapshot &out)
{
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, body);
    if (err) {
        copy_str(out.error, sizeof(out.error), err.c_str());
        return false;
    }

    copy_str(out.plan, sizeof(out.plan), doc["plan"] | "");

    JsonObjectConst lim = doc["limits"];
    out.limitsOk = lim["ok"] | false;
    copy_str(out.status, sizeof(out.status), lim["status"] | "");
    read_window(lim["session"],  out.session);
    read_window(lim["week"],     out.week);
    read_window(lim["weekOpus"], out.weekOpus);
    if (!out.limitsOk) copy_str(out.error, sizeof(out.error), lim["error"] | "");

    JsonObjectConst cost = doc["cost"];
    out.costOk     = cost["ok"] | false;
    out.todayUsd   = cost["todayUsd"] | 0.0f;
    out.monthUsd   = cost["monthUsd"] | 0.0f;
    out.sessionUsd = cost["sessionUsd"] | 0.0f;

    out.dayCount = 0;
    for (JsonObjectConst d : cost["days"].as<JsonArrayConst>()) {
        if (out.dayCount >= MAX_DAYS) break;
        DayCost &slot = out.days[out.dayCount++];
        copy_str(slot.label, sizeof(slot.label), d["label"] | "");
        slot.usd = d["usd"] | 0.0f;
    }

    out.modelCount = 0;
    for (JsonObjectConst m : cost["models"].as<JsonArrayConst>()) {
        if (out.modelCount >= MAX_MODELS) break;
        ModelCost &slot = out.models[out.modelCount++];
        copy_str(slot.name, sizeof(slot.name), m["name"] | "");
        slot.usd = m["usd"] | 0.0f;
        slot.pct = m["pct"] | 0.0f;
    }

    JsonObjectConst tok = cost["tokens"];
    out.tokIn         = tok["in"]         | 0ULL;
    out.tokOut        = tok["out"]        | 0ULL;
    out.tokCacheRead  = tok["cacheRead"]  | 0ULL;
    out.tokCacheWrite = tok["cacheWrite"] | 0ULL;

    out.lastActivitySec = cost["lastActivitySec"] | 0;
    out.ok              = true;
    out.error[0]        = 0;
    return true;
}

static void poll_once()
{
    UsageSnapshot fresh;
    fresh.attemptMs = millis();

    if (net_state() != NetState::Online) {
        copy_str(fresh.error, sizeof(fresh.error), "wifi offline");
    } else if (!g_settings.bridgeHost.length()) {
        copy_str(fresh.error, sizeof(fresh.error), "no bridge configured");
    } else {
        WiFiClient client;
        HTTPClient http;
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.setConnectTimeout(HTTP_TIMEOUT_MS);

        const String url = "http://" + g_settings.bridgeHost + ":" +
                           String(g_settings.bridgePort) + "/usage";

        if (http.begin(client, url)) {
            if (g_settings.bridgeToken.length())
                http.addHeader("Authorization", "Bearer " + g_settings.bridgeToken);

            const int code = http.GET();
            if (code == 200) {
                parse_payload(http.getString(), fresh);
            } else if (code == 401) {
                copy_str(fresh.error, sizeof(fresh.error), "bad bridge token");
            } else if (code > 0) {
                snprintf(fresh.error, sizeof(fresh.error), "bridge HTTP %d", code);
            } else {
                copy_str(fresh.error, sizeof(fresh.error), "bridge unreachable");
            }
            http.end();
        } else {
            copy_str(fresh.error, sizeof(fresh.error), "bad bridge URL");
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (fresh.ok) {
        fresh.fetchedMs = fresh.attemptMs;
        s_snap = fresh;
    } else {
        // Keep the last good numbers on screen; just record why we failed.
        s_snap.ok        = false;
        s_snap.attemptMs = fresh.attemptMs;
        memcpy(s_snap.error, fresh.error, sizeof(s_snap.error));
    }
    xSemaphoreGive(s_lock);
}

static void poll_task(void *)
{
    for (;;) {
        poll_once();
        // Woken early by usage_client_refresh_now().
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_settings.pollMs));
    }
}

void usage_client_begin()
{
    s_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(poll_task, "usage", 8192, nullptr, 3, &s_task, 0);
}

void usage_client_get(UsageSnapshot &out)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out = s_snap;
    xSemaphoreGive(s_lock);
}

void usage_client_refresh_now()
{
    if (s_task) xTaskNotifyGive(s_task);
}
