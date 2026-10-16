#include "wifi_setup.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <vector>

#include "config.h"
#include "input.h"
#include "net.h"
#include "panel.h"
#include "ui.h"
#include "wifi_config.h"
#include "wifi_store.h"

namespace {

enum class Join { IDLE, PENDING, TRYING, OK, FAILED };

DNSServer *dns = nullptr;
WebServer *server = nullptr;

std::vector<String> scanned;   // SSIDs seen in the scan, strongest first
String pendingSsid;
String pendingPass;
Join join = Join::IDLE;
uint32_t joinStart = 0;
String joinedSsid;
String savedNote;   // result of a "save only" or "remove", no join involved
bool screenDirty = true;

void feedWdt() { esp_task_wdt_reset(); }

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String urlEncode(const String &in) {
  const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                c == '~';
    if (safe) {
      out += c;
    } else {
      out += '%';
      out += hex[((unsigned char)c >> 4) & 0x0F];
      out += hex[(unsigned char)c & 0x0F];
    }
  }
  return out;
}

String statusText() {
  if (join == Join::IDLE && savedNote.length()) return savedNote;
  switch (join) {
    case Join::PENDING:
    case Join::TRYING:
      return "Saved. Connecting to " + pendingSsid + " ...";
    case Join::OK:
      return "Connected to " + joinedSsid + ".";
    case Join::FAILED:
      return "Saved, but could not join " + pendingSsid +
             ". Check the password and try again.";
    default:
      return String();
  }
}

// One page, no external assets: the phone that opens the portal has no route
// to the internet through this AP.
String portalPage() {
  String s;
  s.reserve(2048);
  s += "<!doctype html><html><head><meta charset=\"utf-8\">";
  s += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  s += "<title>E-reader WiFi</title><style>";
  s += "body{font:16px system-ui,sans-serif;margin:0;padding:20px;max-width:32em}";
  s += "h1{font-size:1.3em;margin:0 0 .6em}";
  s += "label{display:block;margin:.9em 0 .2em;font-weight:600}";
  s += "input,select,button{font:inherit;width:100%;box-sizing:border-box;padding:.5em}";
  s += "button{margin-top:.8em;background:#111;color:#fff;border:0;padding:.7em}";
  s += "button.alt{background:#fff;color:#111;border:1px solid #111}";
  s += "ul{list-style:none;padding:0;margin:.4em 0}";
  s += "li{padding:.35em 0;border-bottom:1px solid #ddd}";
  s += "li a{float:right;color:#a00;text-decoration:none}";
  s += ".msg{margin:.8em 0;padding:.7em;background:#eee;border-left:4px solid #111}";
  s += ".known{color:#555;font-size:.9em}";
  s += "</style></head><body><h1>E-reader WiFi</h1>";

  String msg = statusText();
  if (msg.length()) s += "<div class=\"msg\">" + htmlEscape(msg) + "</div>";

  s += "<form method=\"POST\" action=\"/save\">";
  s += "<label for=\"pick\">Network</label><select id=\"pick\" ";
  s += "onchange=\"document.getElementById('ssid').value=this.value\">";
  s += "<option value=\"\">choose a network</option>";
  for (size_t i = 0; i < scanned.size(); i++) {
    String e = htmlEscape(scanned[i]);
    s += "<option value=\"" + e + "\">" + e + "</option>";
  }
  s += "</select>";
  s += "<label for=\"ssid\">Network name</label>";
  s += "<input id=\"ssid\" name=\"ssid\" autocapitalize=\"off\" ";
  s += "autocorrect=\"off\" spellcheck=\"false\" required>";
  s += "<label for=\"pw\">Password</label>";
  s += "<input id=\"pw\" name=\"pw\" type=\"password\" autocapitalize=\"off\">";
  s += "<button type=\"submit\" name=\"go\" value=\"connect\">";
  s += "Save and connect</button>";
  s += "<button class=\"alt\" type=\"submit\" name=\"go\" value=\"add\">";
  s += "Save and add another</button></form>";

  std::vector<wifi_store::Net> known = wifi_store::list();
  s += "<p class=\"known\">Saved (" + String((unsigned)known.size()) + "/" +
       String((unsigned)WIFI_STORE_MAX_NETWORKS) + ")</p>";
  if (known.empty()) {
    s += "<p class=\"known\">nothing yet</p>";
  } else {
    s += "<ul class=\"known\">";
    for (size_t i = 0; i < known.size(); i++) {
      String e = htmlEscape(known[i].ssid);
      s += "<li>" + e + "<a href=\"/forget?ssid=" +
           htmlEscape(urlEncode(known[i].ssid)) + "\">remove</a></li>";
    }
    s += "</ul>";
  }
  s += "<p class=\"known\">Screen: " + htmlEscape(panel::name(panel::active())) +
       ". Change it from the reader's menu (Screen).</p>";
  s += "<p class=\"known\">Press the center button on the reader to "
       "finish.</p></body></html>";
  return s;
}

void handleRoot() {
  server->send(200, "text/html", portalPage());
}

void handleSave() {
  String ssid = server->arg("ssid");
  String pass = server->arg("pw");
  ssid.trim();
  if (!ssid.length()) {
    server->send(200, "text/html", portalPage());
    return;
  }
  bool ok = wifi_store::add(ssid, pass);

  // "Save and add another" only writes the store, so a batch of networks can
  // go in during one visit without waiting out a join for each one. Only
  // "Save and connect" arms the join.
  if (server->arg("go") == "add") {
    savedNote = ok ? ("Saved " + ssid + ". Add another.")
                   : ("Could not save " + ssid + ". The store may be full.");
    join = Join::IDLE;
    screenDirty = true;
    server->sendHeader("Location", "/", true);
    server->send(302, "text/plain", "");
    return;
  }

  savedNote = "";
  pendingSsid = ssid;
  pendingPass = pass;
  join = Join::PENDING;
  screenDirty = true;

  // Answer immediately and let the page poll: the join takes seconds and the
  // portal has to stay responsive while it runs.
  String s = "<!doctype html><html><head><meta charset=\"utf-8\">";
  s += "<meta http-equiv=\"refresh\" content=\"3;url=/\">";
  s += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
  s += "<title>Connecting</title></head><body style=\"font:16px system-ui,";
  s += "sans-serif;padding:20px\"><p>Saved ";
  s += htmlEscape(ssid);
  s += ". Connecting ...</p></body></html>";
  server->send(200, "text/html", s);
}

void handleForget() {
  String ssid = server->arg("ssid");
  if (ssid.length() && wifi_store::remove(ssid)) {
    savedNote = "Removed " + ssid + ".";
  } else if (ssid.length()) {
    savedNote = ssid + " was not stored.";
  }
  join = Join::IDLE;
  screenDirty = true;
  server->sendHeader("Location", "/", true);
  server->send(302, "text/plain", "");
}

// Everything else is answered with a redirect to the portal, which is what
// makes phones pop the "sign in to network" sheet.
void handleNotFound() {
  server->sendHeader("Location",
                     String("http://") + WIFI_SETUP_AP_IP_TEXT + "/", true);
  server->send(302, "text/plain", "");
}

// Appends `text` to `lines` broken at spaces to the active panel's text grid
// width (TEXT_COLS: 54 on the 5.76 inch panel, 37 on the 7.5 inch), so a
// long status or a list of saved networks wraps instead of running off the
// glass. A word longer than a line is cut hard.
void pushWrapped(std::vector<String> &lines, const String &text,
                 const String &indent = "") {
  const int cols = TEXT_COLS > 8 ? TEXT_COLS : 8;
  String rest = text;
  bool first = true;
  while (rest.length()) {
    String lead = first ? String() : indent;
    int room = cols - (int)lead.length();
    if ((int)rest.length() <= room) {
      lines.push_back(lead + rest);
      break;
    }
    int cut = rest.lastIndexOf(' ', (unsigned int)room);
    if (cut <= 0) cut = room;
    lines.push_back(lead + rest.substring(0, cut));
    rest = rest.substring(cut);
    rest.trim();
    first = false;
  }
}

void drawScreen() {
  std::vector<String> lines;
  lines.push_back("WiFi setup");
  lines.push_back("");
  lines.push_back("Join      " + String(WIFI_SETUP_AP_SSID));
  lines.push_back("Open      http://" + String(WIFI_SETUP_AP_IP_TEXT));
  lines.push_back("");

  std::vector<wifi_store::Net> known = wifi_store::list();
  String saved = "Saved     ";
  if (known.empty()) {
    saved += "none";
  } else {
    for (size_t i = 0; i < known.size(); i++) {
      if (i) saved += ", ";
      saved += known[i].ssid;
    }
  }
  pushWrapped(lines, saved, "          ");

  String msg = statusText();
  if (msg.length()) {
    lines.push_back("");
    pushWrapped(lines, msg);
  }
  lines.push_back("");
  lines.push_back("Press center to exit.");
  ui::renderStatusScreen("wifi setup", lines);
}

void pumpJoin() {
  if (join == Join::PENDING) {
    join = Join::TRYING;
    joinStart = millis();
    WiFi.begin(pendingSsid.c_str(),
               pendingPass.length() ? pendingPass.c_str() : nullptr);
    return;
  }
  if (join != Join::TRYING) return;

  if (WiFi.status() == WL_CONNECTED) {
    join = Join::OK;
    joinedSsid = pendingSsid;
    screenDirty = true;
    return;
  }
  if (millis() - joinStart >= WIFI_CONNECT_TIMEOUT_MS) {
    join = Join::FAILED;
    WiFi.disconnect(false, false);
    screenDirty = true;
  }
}

}  // namespace

namespace wifi_setup {

void run() {
  scanned.clear();
  pendingSsid = "";
  pendingPass = "";
  joinedSsid = "";
  savedNote = "";
  join = Join::IDLE;

  {
    std::vector<String> lines;
    lines.push_back("WiFi setup");
    lines.push_back("");
    lines.push_back("Scanning for networks ...");
    ui::renderStatusScreen("wifi setup", lines, true);
  }

  // Scan before the AP is up: a soft AP pins the radio to one channel and
  // makes the scan slower and less complete.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  feedWdt();
  int found = WiFi.scanNetworks(false, false);
  feedWdt();
  for (int i = 0; i < found && scanned.size() < 24; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (size_t k = 0; k < scanned.size(); k++) {
      if (scanned[k] == ssid) dup = true;
    }
    if (!dup) scanned.push_back(ssid);
  }
  WiFi.scanDelete();

  WiFi.mode(WIFI_AP_STA);
  IPAddress apIp(192, 168, 4, 1);
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));
  if (strlen(WIFI_SETUP_AP_PASSWORD) >= 8) {
    WiFi.softAP(WIFI_SETUP_AP_SSID, WIFI_SETUP_AP_PASSWORD);
  } else {
    WiFi.softAP(WIFI_SETUP_AP_SSID);
  }

  dns = new DNSServer();
  dns->setErrorReplyCode(DNSReplyCode::NoError);
  dns->start(53, "*", apIp);

  server = new WebServer(80);
  server->on("/", HTTP_GET, handleRoot);
  server->on("/save", HTTP_POST, handleSave);
  server->on("/forget", HTTP_GET, handleForget);
  server->onNotFound(handleNotFound);
  server->begin();

  screenDirty = true;
  while (true) {
    feedWdt();
    dns->processNextRequest();
    server->handleClient();
    pumpJoin();

    if (screenDirty) {
      screenDirty = false;
      drawScreen();
    }

    Button b;
    if (input::poll(b) && b == BTN_CENTER) break;
    delay(BUTTON_POLL_MS);
  }

  server->stop();
  delete server;
  server = nullptr;
  dns->stop();
  delete dns;
  dns = nullptr;

  WiFi.softAPdisconnect(true);
  wifi_off();
  scanned.clear();
}

}  // namespace wifi_setup
