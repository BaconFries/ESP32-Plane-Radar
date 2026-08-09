#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "services/adsb_client.h"
#include "services/flight_track.h"
#include "services/portal_features.h"
#include "services/portal_location_head.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t s_boot_tap_count = 0;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      // Count taps (do not coalesce) so double-tap survives blocking HTTP.
      if (s_boot_tap_count < 10) {
        ++s_boot_tap_count;
      }
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
WiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();

constexpr int kCoordParamLen = 20;
constexpr int kZipParamLen = 10;
constexpr char kCoordInputAttrs[] =
    " type=\"number\" step=\"0.000001\"";
constexpr char kZipInputAttrs[] = " type=\"text\" inputmode=\"numeric\" maxlength=\"10\" placeholder=\"US ZIP (optional if lat/lon set)\"";

WiFiManagerParameter s_param_zip("radar_zip", "US ZIP (fills lat/lon; preferred)", "",
                                 kZipParamLen, kZipInputAttrs);
WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                kCoordParamLen, kCoordInputAttrs);

char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  s_param_zip.setValue(services::location::zip(), kZipParamLen);
  snprintf(s_miles_checkbox_attrs, sizeof(s_miles_checkbox_attrs), "type=\"checkbox\"%s",
           ui::radar::useMiles() ? " checked" : "");
  s_param_miles.setValue("T", 2);
  snprintf(s_runways_checkbox_attrs, sizeof(s_runways_checkbox_attrs),
           "type=\"checkbox\"%s", ui::radar::showRunways() ? " checked" : "");
  s_param_runways.setValue("T", 2);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromPortal(s_param_lat.getValue(),
                                          s_param_lon.getValue(),
                                          s_param_zip.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
}

void appendPortalPageChrome(String& html, const char* title, const char* path) {
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>");
  if (strcmp(path, "/traffic") == 0) {
    html += F("<meta http-equiv='refresh' content='5'>");
  }
  html += F("<title>");
  html += title;
  html += F("</title><style>"
            "body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
            "margin:16px;max-width:720px}"
            "a{color:#8ec8ff}table{width:100%;border-collapse:collapse;margin-top:12px}"
            "th,td{text-align:left;padding:8px 6px;border-bottom:1px solid #333;"
            "font-size:14px}th{color:#9ab}h1{font-size:1.35rem;margin:0 0 6px}"
            "h2{font-size:1.05rem;margin:0 0 8px}"
            ".nav{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 14px}"
            ".nav a{display:inline-block;padding:8px 12px;border-radius:6px;"
            "background:#1a2332;border:1px solid #2a3a50;color:#cde;text-decoration:none;"
            "font-size:14px}"
            ".nav a.on{background:#1fa3ec;border-color:#1fa3ec;color:#fff}"
            ".urlbox{background:#1a2332;border:1px solid #2a3a50;border-radius:8px;"
            "padding:10px 12px;margin:0 0 14px;font-size:13px;color:#9ab}"
            ".urlbox code{color:#8ec8ff;word-break:break-all}"
            ".meta{color:#889;font-size:13px;margin-bottom:8px}"
            ".card{background:#1a2332;border:1px solid #2a3a50;border-radius:8px;"
            "padding:12px;margin:12px 0}"
            "label{display:block;margin:8px 0 4px;font-size:13px;color:#9ab}"
            "input[type=text]{width:100%;padding:10px;border-radius:6px;border:1px solid #345;"
            "background:#0d1520;color:#fff;box-sizing:border-box}"
            "button{margin-top:10px;padding:10px 14px;background:#1fa3ec;color:#fff;"
            "border:0;border-radius:6px;font-size:15px}"
            "</style></head><body>");
  html += F("<p class='nav'>"
            "<a href='/'>Portal home</a>"
            "<a href='/param'>Setup</a>"
            "<a href='/traffic'");
  if (strcmp(path, "/traffic") == 0) {
    html += F(" class='on'");
  }
  html += F(">Live traffic</a>"
            "<a href='/track'");
  if (strcmp(path, "/track") == 0) {
    html += F(" class='on'");
  }
  html += F(">Track flight</a></p>");
  html += F("<h1>");
  html += title;
  html += F("</h1><div class='urlbox'>Bookmark this page:<br><code>http://");
  html += config::kPortalHostUrl;
  html += path;
  html += F("</code>");
  if (wifiLinkUp()) {
    html += F("<br>or <code>http://");
    html += WiFi.localIP().toString();
    html += path;
    html += F("</code>");
  }
  html += F("</div>");
}

void handleTrafficPage() {
  if (!s_wm.server) {
    return;
  }
  String html;
  html.reserve(4600);
  appendPortalPageChrome(html, "Live traffic", "/traffic");
  html += F("<p class='meta'>Aircraft from the last ADS-B fetch · auto-refresh 5s · "
            "pick a callsign then open <a href='/track'>Track flight</a></p>");

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  char meta[80];
  snprintf(meta, sizeof(meta), "<p class='meta'>%u aircraft in last ADS-B fetch</p>",
           static_cast<unsigned>(n));
  html += meta;

  if (n == 0) {
    html += F("<p>No aircraft in range right now.</p>");
  } else {
    html += F("<table><thead><tr>"
              "<th>Flight</th><th>Type</th><th>Alt</th>"
              "<th>Track</th><th>GS</th></tr></thead><tbody>");
    for (size_t i = 0; i < n; ++i) {
      const char* cs =
          planes[i].callsign[0] != '\0' ? planes[i].callsign : "—";
      const char* ty = planes[i].type[0] != '\0' ? planes[i].type : "—";
      const char* alt = planes[i].alt[0] != '\0' ? planes[i].alt : "—";
      char row[160];
      snprintf(row, sizeof(row),
               "<tr><td>%s</td><td>%s</td><td>%s</td><td>%.0f°</td><td>%.0f kt</td></tr>",
               cs, ty, alt, static_cast<double>(planes[i].track_deg),
               static_cast<double>(planes[i].gs_knots));
      html += row;
    }
    html += F("</tbody></table>");
  }
  html += F("</body></html>");
  s_wm.server->send(200, "text/html", html);
}

void appendTrackStatusHtml(String& html) {
  const auto& st = services::flight_track::status();
  if (!st.active) {
    html += F("<p class='meta'>No flight is being tracked.</p>");
    return;
  }
  html += F("<div class='card'><h2>Current track</h2>");
  char line[192];
  snprintf(line, sizeof(line), "<p><b>%s</b> · %s</p>", st.callsign,
           st.phase_label[0] ? st.phase_label : "—");
  html += line;
  if (st.adsb_callsign[0] && strcmp(st.adsb_callsign, st.callsign) != 0) {
    snprintf(line, sizeof(line),
             "<p class='meta'>ADS-B callsign: %s</p>", st.adsb_callsign);
    html += line;
  }
  if (st.airline[0]) {
    snprintf(line, sizeof(line), "<p>%s</p>", st.airline);
    html += line;
  }
  if (st.route_line[0]) {
    snprintf(line, sizeof(line), "<p>%s</p>", st.route_line);
    html += line;
  }
  {
    const char* out_t = st.have_takeoff ? st.takeoff_label : "—";
    if (st.have_landing) {
      snprintf(line, sizeof(line), "<p>Out %s · In %s</p>", out_t,
               st.landing_label);
    } else if (st.eta_label[0]) {
      snprintf(line, sizeof(line), "<p>Out %s · ETA %s</p>", out_t,
               st.eta_label);
    } else {
      snprintf(line, sizeof(line), "<p>Out %s · In —</p>", out_t);
    }
    html += line;
  }
  snprintf(line, sizeof(line),
           "<p>Alt %s · GS %.0f kt · Track %.0f°</p>",
           st.alt[0] ? st.alt : "—", static_cast<double>(st.gs_knots),
           static_cast<double>(st.track_deg));
  html += line;
  if (st.dist_km >= 0.0f) {
    snprintf(line, sizeof(line), "<p>Distance from radar: %.1f km</p>",
             static_cast<double>(st.dist_km));
    html += line;
  }
  html += F("<form method='POST' action='/track' style='margin-top:10px'>"
            "<input type='hidden' name='clear' value='1'>"
            "<button type='submit'>Stop tracking</button></form></div>");
}

void handleTrackStatusFragment() {
  if (!s_wm.server) {
    return;
  }
  String html;
  html.reserve(1024);
  appendTrackStatusHtml(html);
  s_wm.server->send(200, "text/html", html);
}

void handleTrackPage() {
  if (!s_wm.server) {
    return;
  }
  if (s_wm.server->method() == HTTP_POST) {
    if (s_wm.server->hasArg("clear")) {
      services::flight_track::clear();
    } else if (s_wm.server->hasArg("callsign")) {
      const String cs = s_wm.server->arg("callsign");
      if (!services::flight_track::start(cs.c_str())) {
        s_wm.server->send(400, "text/plain", "Invalid flight / callsign");
        return;
      }
    }
    s_wm.server->sendHeader("Location", "/track", true);
    s_wm.server->send(303, "text/plain", "");
    return;
  }

  String html;
  html.reserve(4600);
  appendPortalPageChrome(html, "Track a flight", "/track");
  html += F("<p class='meta'>Enter a flight number (e.g. DL2460) or ADS-B callsign "
            "(e.g. DAL2460). Status updates below without clearing this form. "
            "On the device, double-tap BOOT until the track screen is shown "
            "(or leave Auto mode when the local radar is empty).</p>"
            "<div id='track-status'>");
  appendTrackStatusHtml(html);
  html += F("</div>"
            "<div class='card'><h2>Start tracking</h2>"
            "<form method='POST' action='/track'>"
            "<label for='callsign'>Flight / callsign</label>"
            "<input id='callsign' name='callsign' type='text' maxlength='8' "
            "placeholder='DL2460' autocomplete='off' required>"
            "<button type='submit'>Start tracking</button>"
            "</form></div>"
            "<script>"
            "(function(){"
            "function refresh(){"
            "var el=document.getElementById('track-status');"
            "if(!el)return;"
            "fetch('/track/status').then(function(r){return r.text();})"
            ".then(function(t){el.innerHTML=t;})"
            ".catch(function(){});"
            "}"
            "setInterval(refresh,8000);"
            "})();"
            "</script>"
            "</body></html>");
  s_wm.server->send(200, "text/html", html);
}

void onWebServerReady() {
  if (!s_wm.server) {
    return;
  }
  s_wm.server->on("/traffic", HTTP_GET, handleTrafficPage);
  s_wm.server->on("/track", HTTP_ANY, handleTrackPage);
  s_wm.server->on("/track/status", HTTP_GET, handleTrackStatusFragment);
  Serial.println("Portal: /traffic and /track ready");
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_zip);
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_runways);
  wm.setShowInfoUpdate(false);
  // Own "Setup" page for ZIP/lat/lon (not on the WiFi form).
  // Include "custom" so the features panel is actually rendered on the menu.
  wm.setParamsPage(true);
  std::vector<const char*> menu = {"wifi", "param", "custom", "info", "exit"};
  wm.setMenu(menu);

  static String s_portal_head;
  s_portal_head = String(kPortalFeaturesHeadCss);
  s_portal_head += kPortalLocationHeadHtml;
  wm.setCustomHeadElement(s_portal_head.c_str());
  wm.setCustomMenuHTML(kPortalFeaturesMenuHtml);
  wm.setWebServerCallback(onWebServerReady);
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  Serial.println("WiFi credentials, location, and units cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectSavedNetwork(bool show_ui) {
  if (!storedWifiCredentials()) {
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

uint8_t bootButtonConsumeTapCount() {
  portENTER_CRITICAL(&s_boot_mux);
  const uint8_t n = s_boot_tap_count;
  s_boot_tap_count = 0;
  portEXIT_CRITICAL(&s_boot_mux);
  return n;
}

bool bootButtonConsumeTap() { return bootButtonConsumeTapCount() > 0; }

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
