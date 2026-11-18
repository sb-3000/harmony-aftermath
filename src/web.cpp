#include "web.h"

#include <ArduinoJson.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "activities.h"
#include "ble.h"
#include "config.h"
#include "hub_log.h"
#include "remote.h"
#include "tasmota.h"
#include "wifi_setup.h"

namespace {
constexpr size_t MAX_CONFIG_BYTES = 16384;

WebServer server(80);
bool updateFailed = false;

String htmlEscape(String text) {
  text.replace("&", "&amp;");
  text.replace("<", "&lt;");
  text.replace(">", "&gt;");
  return text;
}

// Names in URL paths may contain spaces or slashes, so callers send them percent-encoded
// (%20, %2F). The web server doesn't decode the path, so each part is decoded after splitting.
String urlDecodePart(const String& text) {
  String decoded;
  decoded.reserve(text.length());
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c == '%' && i + 2 < text.length() && isxdigit(text.charAt(i + 1)) && isxdigit(text.charAt(i + 2))) {
      char hex[3] = {text.charAt(i + 1), text.charAt(i + 2), 0};
      decoded += char(strtol(hex, nullptr, 16));
      i += 2;
    } else {
      decoded += c;
    }
  }
  return decoded;
}

String deviceStateText(const String& name, JsonObject device) {
  const char* host = device["host"];
  if (host != nullptr) {
    TasmotaState state = tasmotaState(host);
    return state.known ? state.power : "unknown";
  }
  const char* stateHost = device["state"]["tasmota"];
  if (stateHost != nullptr) {
    TasmotaState state = tasmotaState(stateHost);
    if (!state.known) return "unknown";
    return state.switch1 == (device["state"]["on_value"] | "") ? "ON" : "OFF";
  }
  if (device["transport"] == "bluetooth") return bleIsConnected(name) ? "connected" : "not connected";
  // No way to read it, so show the last On or Off the hub sent.
  String remembered = devicePowerState(name);
  return remembered.isEmpty() ? "unknown" : remembered + " (last sent)";
}

String remoteAddress() {
  return gConfig["remote"]["address"] | "";
}

bool remotePaired() {
  return remoteAddress().length() == 10;
}

bool saveRemoteAddress(const String& address, int channel, String& error) {
  String current = configText();
  JsonDocument doc;
  if (deserializeJson(doc, current)) {
    error = "No valid config is loaded";
    return false;
  }
  JsonObject remote = doc["remote"];
  if (remote.isNull()) {
    error = "Config is missing the remote section";
    return false;
  }

  remote["address"] = address;
  remote["channel"] = channel;
  String updated;
  serializeJson(doc, updated);
  if (!configReplace(updated, error)) return false;

  if (address.isEmpty()) {
    remoteStopListening();
    remotePairingClearStatus();
    logEvent("Harmony remote unpaired");
  } else {
    remoteListen(strtoull(address.c_str(), nullptr, 16), channel);
    logEvent("Harmony remote paired at %s on channel %d", address.c_str(), channel);
  }
  return true;
}

void fillStatus(JsonDocument& doc) {
  RemoteStatus remote = remoteStatus();
  doc["version"] = HUB_VERSION;
  doc["uptime_s"] = millis() / 1000;
  doc["activity"] = activityCurrent();
  doc["activity_running"] = activityBusy();
  doc["wifi"]["connected"] = WiFi.status() == WL_CONNECTED;
  doc["wifi"]["ssid"] = WiFi.SSID();
  doc["wifi"]["setup_hotspot"] = wifiHotspotActive();
  doc["wifi"]["ip"] = WiFi.localIP().toString();
  doc["wifi"]["rssi"] = WiFi.RSSI();
  doc["remote"]["radio_ok"] = remote.radioOk;
  doc["remote"]["paired"] = remotePaired();
  doc["remote"]["name"] = "Harmony Companion";
  doc["remote"]["address"] = remoteAddress();
  doc["remote"]["channel"] = gConfig["remote"]["channel"] | 5;
  doc["remote"]["packets"] = remote.packets;
  doc["remote"]["bad_checksums"] = remote.badChecksums;
  doc["remote"]["last_button"] = lastButtonText();
  doc["firmware_slot"] = esp_ota_get_running_partition()->label;
  doc["free_heap"] = ESP.getFreeHeap();
  for (JsonPair pair : gConfig["devices"].as<JsonObject>()) {
    doc["devices"][pair.key()] = deviceStateText(pair.key().c_str(), pair.value());
  }
}

// While the setup hotspot is open, phones probe random addresses to detect a sign-in page.
// Sends anything that arrived over the hotspot for another host to the Wi-Fi page.
bool captiveRedirect() {
  if (!wifiHotspotActive() || server.client().localIP() != WiFi.softAPIP()) return false;
  String ip = WiFi.softAPIP().toString();
  if (server.hostHeader() == ip) return false;
  server.sendHeader("Location", "http://" + ip + "/wifi", true);
  server.send(302, "text/plain", "");
  return true;
}

const char WIFI_PAGE[] PROGMEM = R"html(<!doctype html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>Wi-Fi - Harmony Aftermath</title>
<style>body{font:16px system-ui,sans-serif;margin:16px;max-width:520px}
button{font:inherit;padding:8px 12px}label{display:block;margin:8px 0}
input[type=text],input[type=password]{font:inherit;padding:8px;width:100%;box-sizing:border-box;margin-top:4px}
#nets button{display:flex;justify-content:space-between;width:100%;margin:4px 0;text-align:left}
.dim{color:#777}#result{font-weight:bold}</style></head><body>
<h1>Wi-Fi</h1><p id='status'>Checking...</p>
<h2>Networks</h2><div id='nets' class='dim'>Scanning...</div><p><button id='rescan' hidden>Scan again</button></p>
<h2>Connect</h2><form id='form'>
<label>Network name<input type='text' id='ssid' name='ssid' maxlength='32' required autocapitalize='off' autocorrect='off'></label>
<label>Password<input type='password' id='password' name='password' maxlength='64'></label>
<label><input type='checkbox' id='show'> Show password</label>
<p><button type='submit'>Save and connect</button></p></form>
<p id='result'></p><p><a href='/'>Status</a></p>
<script>
const $=id=>document.getElementById(id);
const esc=x=>{const d=document.createElement('div');d.textContent=x;return d.innerHTML};
const bars=r=>r>-60?'&#9602;&#9604;&#9606;&#9608;':r>-70?'&#9602;&#9604;&#9606;':r>-80?'&#9602;&#9604;':'&#9602;';
async function status(){try{const s=await(await fetch('/wifi/status')).json();
let t=s.connected?`Connected to <b>${esc(s.ssid)}</b> at <b>${s.ip}</b>, ${s.rssi} dBm.`:
s.saved?`Not connected. Trying <b>${esc(s.saved)}</b>.`:'No network saved yet.';
if(s.hotspot)t+=`<br>Setup hotspot <b>${esc(s.hotspot_name)}</b> is open.`;$('status').innerHTML=t}catch(e){}}
function show(list){const d=$('nets');d.innerHTML='';d.className='';
if(!list.length){d.textContent='No networks found.';return}
for(const n of list){const b=document.createElement('button');b.type='button';
b.innerHTML=`<span>${esc(n.ssid)}${n.secure?' &#128274;':''}</span><span class='dim'>${bars(n.rssi)} ${n.rssi} dBm</span>`;
b.onclick=()=>{$('ssid').value=n.ssid;$('password').value='';$('password').focus()};d.appendChild(b)}}
async function scan(){const d=$('nets');d.className='dim';d.textContent='Scanning...';$('rescan').hidden=true;
let url='/wifi/scan?refresh=1';try{for(let i=0;i<20;i++){try{const r=await(await fetch(url)).json();
if(r.networks){show(r.networks);return}}catch(e){}url='/wifi/scan';await new Promise(r=>setTimeout(r,1500))}
d.textContent='Scan failed, try again.'}finally{$('rescan').hidden=false}}
$('rescan').onclick=scan;
$('show').onchange=e=>$('password').type=e.target.checked?'text':'password';
$('form').onsubmit=async e=>{e.preventDefault();$('result').textContent='Saving...';
try{const r=await fetch('/wifi',{method:'POST',body:new URLSearchParams(new FormData($('form')))});
$('result').textContent=await r.text()}catch(e){$('result').textContent='The hub stopped answering. It is probably switching networks.'}};
status();setInterval(status,4000);scan();
</script></body></html>)html";

void handleWifiSave() {
  String ssid = server.arg("ssid");
  String error;
  if (!wifiChangeNetwork(ssid, server.arg("password"), error)) {
    server.send(400, "text/plain", error + "\n");
    return;
  }
  logEvent("Wi-Fi network changed to %s", ssid.c_str());
  server.send(200, "text/plain",
              "Saved. The hub is joining " + ssid +
                  " now. Once it's connected, open http://harmony-aftermath.local/ or the address your router "
                  "gives it. If it can't connect within a minute, the \"" HUB_NAME
                  "\" hotspot opens so you can try again.\n");
}

void handleRoot() {
  if (captiveRedirect()) return;
  JsonDocument doc;
  fillStatus(doc);

  if (server.arg("format") == "json") {
    String body;
    serializeJson(doc, body);
    server.send(200, "application/json", body);
    return;
  }

  String page =
      "<!doctype html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='5'><title>" HUB_NAME "</title>"
      "<style>body{font:16px system-ui,sans-serif;margin:16px;max-width:520px}"
      "td{padding:4px 12px 4px 0}h2{margin-top:24px}</style></head><body>";
  page += "<h1>" HUB_NAME " " HUB_VERSION "</h1><table>";

  auto row = [&page](const String& name, const String& value) {
    page += "<tr><td>" + htmlEscape(name) + "</td><td><b>" + htmlEscape(value) + "</b></td></tr>";
  };

  String activity = activityCurrent();
  row("Activity", activity.isEmpty() ? "none" : activity + (activityBusy() ? " (starting)" : ""));
  String wifiText = doc["wifi"]["connected"] ? WiFi.SSID() + ", " + doc["wifi"]["ip"].as<String>() + ", " +
                                                   WiFi.RSSI() + " dBm"
                                             : "not connected";
  if (wifiHotspotActive()) wifiText += ", setup hotspot open";
  row("Wi-Fi", wifiText);
  row("Radio", doc["remote"]["radio_ok"] ? "OK" : "not responding");
  String remoteText = "Not paired";
  if (doc["remote"]["paired"]) remoteText = "Paired - " + doc["remote"]["address"].as<String>();
  row("Harmony remote", remoteText);
  row("Last button", lastButtonText().isEmpty() ? "none yet" : lastButtonText());
  row("Packets", doc["remote"]["packets"].as<String>() + ", " + doc["remote"]["bad_checksums"].as<String>() + " bad");
  row("Uptime", String(millis() / 1000) + " s");
  row("Firmware slot", doc["firmware_slot"].as<String>());
  page += "</table><h2>Devices</h2><table>";
  for (JsonPair pair : doc["devices"].as<JsonObject>()) {
    row(pair.key().c_str(), pair.value().as<String>());
  }
  page += "</table><h2>Activities</h2><ul>";
  for (JsonPair pair : gConfig["activities"].as<JsonObject>()) {
    page += "<li>" + htmlEscape(pair.key().c_str()) + "</li>";
  }
  page +=
      "</ul><p><a href='/remote'>Remote</a> &middot; <a href='/devices'>Devices</a> &middot; "
      "<a href='/wifi'>Wi-Fi</a> &middot; <a href='/config'>Config</a> &middot; "
      "<a href='/log'>Log</a></p></body></html>";
  server.send(200, "text/html", page);
}

void handleDevicesState() {
  JsonDocument doc;
  doc["remote"]["paired"] = remotePaired();
  doc["remote"]["name"] = "Harmony Companion";
  doc["remote"]["address"] = remoteAddress();
  doc["remote"]["channel"] = gConfig["remote"]["channel"] | 5;
  RemoteStatus remote = remoteStatus();
  doc["remote"]["radio_ok"] = remote.radioOk;
  doc["remote"]["packets"] = remote.packets;
  doc["remote"]["last_button"] = lastButtonText();
  doc["remote"]["pairing"] = remote.pairing;
  doc["remote"]["pairing_state"] = remote.pairingState;
  doc["remote"]["pairing_step"] = remote.pairingStep;
  doc["remote"]["pairing_progress"] = remote.pairingProgress;
  doc["remote"]["pairing_message"] = remote.pairingMessage;
  doc["pairing"] = blePairingName();
  doc["seconds_left"] = blePairingSecondsLeft();
  JsonArray bluetooth = doc["bluetooth"].to<JsonArray>();
  JsonArray others = doc["others"].to<JsonArray>();
  for (JsonPair pair : gConfig["devices"].as<JsonObject>()) {
    String name = pair.key().c_str();
    String transport = pair.value()["transport"] | "";
    if (transport == "bluetooth") {
      JsonObject item = bluetooth.add<JsonObject>();
      item["name"] = name;
      item["paired"] = bleIsPaired(name);
      item["connected"] = bleIsConnected(name);
    } else {
      JsonObject item = others.add<JsonObject>();
      item["name"] = name;
      item["transport"] = transport;
      item["state"] = deviceStateText(name, pair.value());
    }
  }
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

const char DEVICES_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'><title>Devices</title>
<style>body{font:16px system-ui,sans-serif;margin:16px;max-width:520px}td{padding:6px 12px 6px 0}
h2{margin-top:24px}button{font:inherit;padding:6px 12px;margin:2px}
#message{min-height:1.5em}#remote-card{padding:12px;border:1px solid #bbb;border-radius:8px}
.hint{font-size:.9em;color:#555}progress{width:100%}</style></head><body>
<h1>Devices</h1><p id='message'></p>
<h2>Harmony remote</h2><section id='remote-card'><p id='remote-status'>Loading…</p><div id='remote-actions'></div>
<progress id='remote-progress' max='100' value='0' hidden></progress></section>
<h2>Bluetooth</h2><table id='bluetooth'></table>
<h2>Other devices</h2><table id='others'></table><p><a href='/'>Status</a></p>
<script>
const message = document.getElementById('message');
let remoteData = {};
async function act(path, confirmText) {
  if (confirmText && !confirm(confirmText)) return;
  const response = await fetch(path, {method: 'POST'});
  if (!response.ok) message.textContent = await response.text();
  refresh();
}
function cell(row, text) { const td = row.insertCell(); td.textContent = text; return td; }
function button(td, label, path, confirmText) {
  const b = document.createElement('button');
  b.textContent = label;
  b.onclick = () => act(path, confirmText);
  td.appendChild(b);
}
async function refresh() {
  const data = await (await fetch('/devices/state')).json();
  remoteData = data.remote;
  const remoteStatus = document.getElementById('remote-status');
  const remoteActions = document.getElementById('remote-actions');
  const progress = document.getElementById('remote-progress');
  remoteActions.innerHTML = '';
  if (remoteData.pairing) {
    remoteStatus.textContent = remoteData.pairing_message || 'Discovering Harmony remote…';
    progress.hidden = false;
    progress.value = remoteData.pairing_progress || 0;
    const cancel = document.createElement('button');
    cancel.textContent = 'Cancel discovery';
    cancel.onclick = () => act('/remote/cancel');
    remoteActions.appendChild(cancel);
  } else if (remoteData.paired) {
    progress.hidden = true;
    const failure = remoteData.pairing_state === 'failed' ? remoteData.pairing_message + ' ' : '';
    const cancelled = remoteData.pairing_state === 'cancelled' ? 'Discovery cancelled. ' : '';
    remoteStatus.textContent = failure + cancelled + remoteData.name + ' paired · address ' + remoteData.address + ' · channel ' + remoteData.channel +
      (remoteData.radio_ok ? ' · radio listening' : ' · radio not responding') +
      (remoteData.last_button ? ' · last button ' + remoteData.last_button : ' · press a button to check reception');
    const replace = document.createElement('button');
    replace.textContent = 'Replace remote';
    replace.disabled = !remoteData.radio_ok;
    replace.onclick = () => act('/remote/replace', 'Replace the paired remote? Keep the remote nearby; discovery can take a few minutes.');
    remoteActions.appendChild(replace);
    const unpair = document.createElement('button');
    unpair.textContent = 'Unpair';
    unpair.onclick = () => act('/remote/unpair', 'Unpair the Harmony remote?');
    remoteActions.appendChild(unpair);
  } else {
    progress.hidden = true;
    remoteStatus.textContent = remoteData.pairing_message || 'No Harmony remote paired.';
    const pair = document.createElement('button');
    pair.textContent = 'Pair remote';
    pair.disabled = !remoteData.radio_ok;
    pair.onclick = () => act('/remote/pair');
    remoteActions.appendChild(pair);
  }
  const bluetooth = document.getElementById('bluetooth');
  bluetooth.innerHTML = '';
  if (!data.bluetooth.length) cell(bluetooth.insertRow(), 'None in the config');
  for (const d of data.bluetooth) {
    const row = bluetooth.insertRow();
    cell(row, d.name);
    const pairing = data.pairing === d.name;
    cell(row, pairing ? 'Pairing' : d.connected ? 'Paired, connected' : d.paired ? 'Paired' : 'Not paired');
    const td = cell(row, '');
    if (pairing) button(td, 'Cancel', '/bluetooth/cancel');
    else if (d.paired) button(td, 'Forget', '/bluetooth/forget/' + encodeURIComponent(d.name), 'Forget ' + d.name + '?');
    else if (!data.pairing) button(td, 'Pair', '/bluetooth/pair/' + encodeURIComponent(d.name));
  }
  if (data.pairing) message.textContent = 'On ' + data.pairing + ', add a Bluetooth device and choose this hub. ' + data.seconds_left + ' s left.';
  else if (message.textContent.includes(' s left.')) message.textContent = '';
  const others = document.getElementById('others');
  others.innerHTML = '';
  for (const d of data.others) { const row = others.insertRow(); cell(row, d.name); cell(row, d.transport); cell(row, d.state); }
}
refresh();
setInterval(refresh, 2000);
</script></body></html>)HTML";

void handleDevices() {
  server.send_P(200, "text/html", DEVICES_PAGE);
}

void handleConfigGet() {
  server.send(200, "application/json", configText());
}

void handleConfigPut() {
  if (remoteStatus().pairing) {
    server.send(409, "text/plain", "Cancel remote discovery before replacing the config\n");
    return;
  }
  String body = server.arg("plain");
  if (body.isEmpty()) {
    server.send(400, "text/plain", "empty body, send it with Content-Type: application/json\n");
    return;
  }
  if (body.length() > MAX_CONFIG_BYTES) {
    server.send(413, "text/plain", "config is too large\n");
    return;
  }

  String error;
  if (!configReplace(body, error)) {
    logEvent("Config rejected: %s", error.c_str());
    server.send(400, "text/plain", error + "\n");
    return;
  }

  if (remotePaired()) {
    remoteListen(strtoull(remoteAddress().c_str(), nullptr, 16), gConfig["remote"]["channel"] | 5);
  } else {
    remoteStopListening();
  }
  remotePairingClearStatus();
  activitiesConfigChanged();
  server.send(200, "text/plain", "OK\n");
}

void handleApiState() {
  JsonDocument doc;
  String current = activityCurrent();
  doc["activity"] = current;
  doc["busy"] = activityBusy();
  doc["last_button"] = lastButtonText();
  JsonArray activities = doc["activities"].to<JsonArray>();
  for (JsonPair pair : gConfig["activities"].as<JsonObject>()) activities.add(pair.key());
  JsonArray buttons = doc["buttons"].to<JsonArray>();
  for (JsonPair pair : gConfig["remote"]["buttons"].as<JsonObject>()) buttons.add(pair.value());
  String body;
  serializeJson(doc, body);
  server.send(200, "application/json", body);
}

const char REMOTE_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'><title>Remote</title>
<style>body{font:17px system-ui,sans-serif;margin:16px auto;max-width:420px;padding:0 16px}
button{font:inherit;width:100%;padding:16px;margin:6px 0;border-radius:12px;border:1px solid #888;background:#f4f4f4;color:#111}
button.on{background:#2b6cb0;color:#fff;border-color:#2b6cb0}button:active{filter:brightness(.85)}
.row{display:flex;gap:10px}.row button{flex:1}#status{min-height:1.5em;color:#555}
@media(prefers-color-scheme:dark){body{background:#111;color:#eee}button{background:#222;color:#eee;border-color:#555}#status{color:#aaa}}
</style></head><body><h1>Remote</h1><p id='status'>Loading…</p><div id='activities'></div>
<h2>Volume</h2><div class='row'><button data-button='vol_down' data-repeat>Vol −</button>
<button data-button='mute'>Mute</button><button data-button='vol_up' data-repeat>Vol +</button></div>
<p><a href='/'>Status</a></p>
<script>
const status = document.getElementById('status');
async function post(path) {
  const response = await fetch(path, {method: 'POST'});
  if (!response.ok) status.textContent = await response.text();
  return response.ok;
}
function bindButton(el) {
  const path = '/api/button/' + encodeURIComponent(el.dataset.button);
  let timer = null;
  const stop = () => { clearInterval(timer); timer = null; };
  el.addEventListener('pointerdown', e => {
    e.preventDefault();
    post(path);
    if (el.dataset.repeat !== undefined) timer = setInterval(() => post(path), 200);
  });
  ['pointerup', 'pointerleave', 'pointercancel'].forEach(t => el.addEventListener(t, stop));
}
document.querySelectorAll('[data-button]').forEach(bindButton);
async function refresh() {
  const data = await (await fetch('/api/state')).json();
  status.textContent = data.activity ? 'Now: ' + data.activity + (data.busy ? ' (starting)' : '') : 'No activity running';
  const box = document.getElementById('activities');
  box.innerHTML = '';
  for (const a of data.activities) {
    const b = document.createElement('button');
    b.textContent = a;
    if (a === data.activity) b.className = 'on';
    b.onclick = async () => { if (await post('/api/activity/' + encodeURIComponent(a))) refresh(); };
    box.appendChild(b);
  }
}
refresh();
setInterval(refresh, 3000);
</script></body></html>)HTML";

void handleRemotePage() {
  server.send_P(200, "text/html", REMOTE_PAGE);
}

// Handles the POST routes with names in the path: /api/..., /remote/... and /bluetooth/...
void handleNotFound() {
  if (captiveRedirect()) return;
  String uri = server.uri();
  String error;

  if (server.method() == HTTP_POST && uri.startsWith("/api/button/")) {
    if (!activitiesPressButton(urlDecodePart(uri.substring(12)), error)) {
      server.send(400, "text/plain", error + "\n");
      return;
    }
    server.send(200, "text/plain", "OK\n");
    return;
  }

  if (server.method() == HTTP_POST && uri.startsWith("/api/activity/")) {
    if (!activityStart(urlDecodePart(uri.substring(14)), error)) {
      server.send(404, "text/plain", error + "\n");
      return;
    }
    server.send(200, "text/plain", "OK\n");
    return;
  }

  if (server.method() == HTTP_POST && uri.startsWith("/remote/")) {
    String action = uri.substring(8);
    if (action == "pair" || action == "replace") {
      if (!remotePairingStart(action == "replace", error)) {
        server.send(409, "text/plain", error + "\n");
        return;
      }
      server.send(202, "text/plain", "Remote discovery started\n");
      return;
    }
    if (action == "cancel") {
      remotePairingCancel();
      server.send(200, "text/plain", "Discovery cancelled\n");
      return;
    }
    if (action == "unpair") {
      if (remoteStatus().pairing) {
        server.send(409, "text/plain", "Cancel remote discovery before unpairing\n");
        return;
      }
      if (!saveRemoteAddress("", gConfig["remote"]["channel"] | 5, error)) {
        server.send(400, "text/plain", error + "\n");
        return;
      }
      server.send(200, "text/plain", "Remote unpaired\n");
      return;
    }
    server.send(404, "text/plain", "unknown remote action\n");
    return;
  }

  if (server.method() == HTTP_POST && uri.startsWith("/api/device/")) {
    int slash = uri.indexOf('/', 12);
    if (slash < 0) {
      server.send(400, "text/plain", "use /api/device/{name}/{command}\n");
      return;
    }
    if (!deviceCommand(urlDecodePart(uri.substring(12, slash)), urlDecodePart(uri.substring(slash + 1)), error)) {
      server.send(404, "text/plain", error + "\n");
      return;
    }
    server.send(200, "text/plain", "OK\n");
    return;
  }

  if (server.method() == HTTP_POST && uri.startsWith("/bluetooth/")) {
    String action = uri.substring(11);
    bool ok = true;
    if (action == "cancel") {
      bleCancelPair();
    } else if (action.startsWith("pair/") || action.startsWith("forget/")) {
      bool pairing = action.startsWith("pair/");
      String name = urlDecodePart(action.substring(pairing ? 5 : 7));
      if (gConfig["devices"][name]["transport"] != "bluetooth") {
        error = name + " is not a Bluetooth device in the config";
        ok = false;
      } else {
        ok = pairing ? blePair(name, error) : bleForget(name, error);
      }
    } else {
      error = "unknown Bluetooth action";
      ok = false;
    }
    if (!ok) {
      server.send(400, "text/plain", error + "\n");
      return;
    }
    server.send(200, "text/plain", "OK\n");
    return;
  }

  server.send(404, "text/plain", "not found\n");
}

void handleUpdateDone() {
  if (updateFailed) {
    server.send(500, "text/plain", String("update failed: ") + Update.errorString() + "\n");
    return;
  }
  server.send(200, "text/plain", "OK, restarting\n");
  logEvent("Firmware updated, restarting");
  delay(500);
  ESP.restart();
}

void handleUpdateUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    updateFailed = false;
    logEvent("Firmware update starting");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) updateFailed = true;
    return;
  }
  if (updateFailed) return;

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) updateFailed = true;
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) updateFailed = true;
    logEvent("Firmware received: %u bytes, %s", upload.totalSize, updateFailed ? "failed" : "verified");
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    updateFailed = true;
    logEvent("Firmware update aborted");
  }
}
}  // namespace

void webBegin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/devices", HTTP_GET, handleDevices);
  server.on("/remote", HTTP_GET, handleRemotePage);
  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/devices/state", HTTP_GET, handleDevicesState);
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_PUT, handleConfigPut);
  server.on("/wifi", HTTP_GET, []() { server.send_P(200, "text/html", WIFI_PAGE); });
  server.on("/wifi", HTTP_POST, handleWifiSave);
  server.on("/wifi/status", HTTP_GET, []() { server.send(200, "application/json", wifiStatusJson()); });
  server.on("/wifi/scan", HTTP_GET,
            []() { server.send(200, "application/json", wifiScanJson(server.hasArg("refresh"))); });
  server.on("/log", HTTP_GET, []() { server.send(200, "text/plain", logDump()); });
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound(handleNotFound);
  server.begin();
}

void webLoop() {
  server.handleClient();
}
