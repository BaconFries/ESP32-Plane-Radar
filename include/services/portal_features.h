#pragma once

// Injected into WiFiManager menu via setCustomMenuHTML (requires "custom" in setMenu).
// Keep compact — served from RAM on ESP32-C3.
static const char kPortalFeaturesMenuHtml[] = R"PRMENU(
<div class="pr-box">
  <h3>Plane Radar</h3>
  <p class="pr-lead">Open these pages from any device on the same Wi‑Fi (or while connected to the setup AP).</p>
  <div class="pr-feat">
    <a class="pr-btn" href="/traffic">Live traffic</a>
    <p class="pr-url"><code>http://plane-radar.local/traffic</code></p>
    <p class="pr-desc">Aircraft from the last ADS‑B fetch (callsign, type, alt, track, speed). Auto-refreshes.</p>
  </div>
  <div class="pr-feat">
    <a class="pr-btn" href="/track">Track a flight</a>
    <p class="pr-url"><code>http://plane-radar.local/track</code></p>
    <p class="pr-desc">Follow one flight number or callsign on the round display when the local radar is empty.</p>
  </div>
  <div class="pr-feat">
    <p class="pr-label">Wi‑Fi &amp; location</p>
    <p class="pr-desc">Use <b>Setup</b> above for ZIP or lat/lon, miles/km, and runway overlay. <b>Configure WiFi</b> changes the home network.</p>
  </div>
  <div class="pr-tips">
    <p class="pr-label">On the device (BOOT button)</p>
    <ul>
      <li><b>Tap</b> — cycle radar range</li>
      <li><b>Double-tap</b> — Auto → Clock → Track → Auto</li>
      <li><b>Hold 3 s</b> — clear Wi‑Fi &amp; reopen this portal</li>
    </ul>
    <p class="pr-meta">If <code>plane-radar.local</code> does not open, use the device IP from your router or the serial log (setup AP: <code>http://192.168.4.1</code>).</p>
  </div>
</div>
)PRMENU";

// Extra CSS for the menu features box (appended with ZIP head script).
static const char kPortalFeaturesHeadCss[] = R"PRCSS(
<style>
.pr-box{margin:12px 0 16px;padding:14px;background:#1a2332;border:1px solid #2a3a50;border-radius:10px;text-align:left}
.pr-box h3{margin:0 0 6px;font-size:1.15rem;color:#fff}
.pr-lead,.pr-desc,.pr-meta{color:#9ab;font-size:13px;line-height:1.4;margin:0 0 8px}
.pr-feat{margin:12px 0;padding-top:10px;border-top:1px solid #2a3a50}
.pr-feat:first-of-type{border-top:0;padding-top:4px}
.pr-btn{display:inline-block;margin:0 0 6px;padding:10px 14px;background:#1fa3ec;color:#fff!important;text-decoration:none;border-radius:6px;font-weight:600;font-size:15px}
.pr-url{margin:0 0 4px}
.pr-url code,.pr-meta code{font-size:12px;color:#8ec8ff;word-break:break-all}
.pr-label{margin:0 0 4px;font-weight:600;color:#cde;font-size:14px}
.pr-tips{margin-top:12px;padding-top:10px;border-top:1px solid #2a3a50}
.pr-tips ul{margin:4px 0 8px 1.1em;padding:0;color:#9ab;font-size:13px;line-height:1.45}
.pr-tips li{margin:2px 0}
</style>
)PRCSS";
