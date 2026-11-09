#pragma once

#include <Arduino.h>

// Books and games server, reached from the menu ("Books and games (WiFi)").
//
// Blocks until the user presses CENTER or the server has seen no request for
// GAMES_IDLE_TIMEOUT_MS. Joins the stored WiFi (if any) and raises the
// GAMES_AP_SSID access point, then serves the browser emulator page embedded
// in the firmware (www/dist, see platformio.ini board_build.embed_files), the
// ROMs under GAMES_ROM_DIR and the save files under GAMES_SAVE_DIR. All
// emulation happens in the browser; the reader only stores bytes. The same
// page adds books (a .txt is paginated on the device, a .pgs is stored as is)
// and deletes them. The HTTP
// contract is documented in SPEC.md ("Games server"). The radio is off when
// this returns.

namespace games {

void run();

// Number of ROM files under GAMES_ROM_DIR, for the menu tile.
size_t romCount();

// Number of books under BOOKS_DIR, for the menu tile and the server screen.
size_t bookCount();

// Radio bring-up shared by the modes that serve HTTP on the reader (this
// server). netUp() joins the
// strongest stored network if there is one, raises the GAMES_AP_SSID access
// point at GAMES_AP_IP_TEXT alongside it, and announces GAMES_MDNS_HOST.local
// when the station link came up. netDown() undoes all of it and leaves the
// radio off. The caller owns the WebServer.
struct NetLink {
  String ssid;        // joined network, empty when AP only
  bool mdns = false;  // GAMES_MDNS_HOST.local announced
};
NetLink netUp();
void netDown(const NetLink &link);

// Widens the task watchdog to NEWS_WDT_TIMEOUT_MS for its lifetime: a server
// loop cannot feed it while handleClient() streams a script to a phone on a
// weak link or reads a slow body. Same trick as the news sync.
struct WdtWindow {
  WdtWindow();
  ~WdtWindow();
};

}  // namespace games
