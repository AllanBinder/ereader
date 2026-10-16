#include "pokemon_views.h"

#include <esp_task_wdt.h>
#include <time.h>

#include "config.h"
#include "gbgfx.h"
#include "pack.h"
#include "pokemon_state.h"
#include "ui.h"

// The four views of docs/pokemon-aux-display-layout.md. Coordinates, scales
// and field order follow that document; where this file departs from it, the
// reason is here:
//
// 1. Wild encounter slot odds are 20 20 15 10 10 10 5 5 4 1 percent, not the
//    layout doc's 20 20 15 10 10 10 5 5 1 1 (which sums to 97) nor the brief's
//    20 20 10 10 10 10 5 5 4 1 (which sums to 95). pokered's cumulative table
//    in engine/battle/wild_mons.asm is 51 102 141 166 191 216 229 242 253 255,
//    whose deltas over 256 round to the set used here and sum to 100.
// 2. Encounter rows merge slots by species (the doc merges by species and
//    level) and print the level column as a three cell field, "  3" or "3-5",
//    so a merged range still fits. The row is still 18 cells wide.
// 3. Types and the catch rate come from the live battle and party structs
//    rather than from the pack's base stats. They are the same numbers in
//    every ordinary case, they cost no pack read, and the struct is right when
//    a battle has changed a type (Conversion).
// 4. The party detail line's species name comes from the pack by dex number,
//    as the doc says, but its OT id is the party struct's own OT id word.
// 5. The badge row's earned / unearned art is the pack's `badges` and
//    `leader_faces` sections, which the generator already split for us, rather
//    than one sheet indexed with the game's tile id + 4.
// 6. The battle view's enemy "base stats" line is the pack's base stats for
//    the species, which is what the doc asks for; the enemy's own in battle
//    stat words are not shown (the player's are).

namespace {

using pokemon::BattleMon;
using pokemon::Mon;

// ---------------------------------------------------------------- geometry

// Every coordinate in this file is in the 680x920 design space of the layout
// doc; gbgfx::setLayout() maps it onto the active canvas (identity on panel
// 0, a 480/680 shrink to 480x649 on the 7.5 inch panel). Draw through gbgfx
// (printGb, drawTile*, gbgfx::fillRect) rather than ui::gfx() so a coordinate
// is never read as a panel pixel. The only panel space pieces are the two
// black bands, which run the full canvas width, and the hint band, which is
// pinned to the bottom of the canvas.
const int DESIGN_W = 680;
const int DESIGN_H = 920;
const int TOP_BAR_H = 24;
const int HINT_Y = DESIGN_H - 24;   // 896
const int HINT_TEXT_Y = HINT_Y + 4;  // 900
const int BAR_TEXT_Y = 4;

// ---------------------------------------------------------------- sections

const char *S_FONT_BATTLE = "font_battle";
const char *S_TOWN_TILES = "townmap_tiles";
const char *S_TOWN_MAP = "townmap_map";
const char *S_TOWN_CURSOR = "townmap_cursor";
const char *S_TOWN_ENTRIES = "townmap_entries";
const char *S_MAP_NAMES = "map_names";
const char *S_MAP_LABELS = "map_labels";
const char *S_BADGES = "badges";
const char *S_LEADER_FACES = "leader_faces";
const char *S_BADGE_NUMBERS = "badge_numbers";
const char *S_ICONS = "icons";
const char *S_ICON_MAP = "icon_map";
const char *S_FRONT = "front";
const char *S_FRONT_INDEX = "front_index";
const char *S_BACK = "back";
const char *S_BACK_INDEX = "back_index";
const char *S_PLAYER_WALK = "player_walk";
const char *S_TILESETS = "tilesets";
const char *S_TILESET_INDEX = "tileset_index";
const char *S_BLOCKSETS = "blocksets";
const char *S_BLOCKSET_INDEX = "blockset_index";
const char *S_MAPS = "maps";
const char *S_MAP_INDEX = "map_index";
const char *S_DEX_ORDER = "dex_order";
const char *S_SPECIES_NAMES = "species_names";
const char *S_BASE_STATS = "base_stats";
const char *S_ITEM_NAMES = "item_names";
const char *S_ITEM_PRICES = "item_prices";
const char *S_MOVE_NAMES = "move_names";
const char *S_MOVE_DATA = "move_data";
const char *S_TYPE_NAMES = "type_names";
const char *S_TYPE_CHART = "type_chart";
const char *S_TRAINER_CLASS = "trainer_class";
const char *S_WILD = "wild";
const char *S_ITEMS_PLACED = "items_placed";
const char *S_ITEMS_HIDDEN = "items_hidden";

// Gen 1 character codes the layout doc names. The first block lives in the
// 2 bpp font_battle page (index = code - 0x60); the accented e is an ordinary
// glyph of the 1 bpp font page.
const uint8_t TILE_BAR_LEFT = 0x62;
const uint8_t TILE_BAR_EMPTY = 0x63;   // 0x63 + n is a fill of n pixels
const uint8_t TILE_BAR_FULL = 0x6B;
const uint8_t TILE_BAR_CAP_PARTY = 0x6C;
const uint8_t TILE_BAR_CAP_BATTLE = 0x6D;
const uint8_t TILE_LV = 0x6E;
const uint8_t TILE_HP = 0x71;
const uint8_t TILE_ID = 0x73;
const uint8_t TILE_NO = 0x74;
const uint8_t CODE_E_ACUTE = 0xBA;
const uint8_t CODE_RULE = 0x7A;   // the horizontal box rule

// ------------------------------------------------------------------ caches
//
// Everything here is read from the pack on demand. The budget is the 8 KB
// work RAM snapshot and gbgfx's 2 KB font page plus what is below, which is
// about 11 KB: one tileset, one blockset, the town map, the battle font page,
// the type chart and one Pokemon picture at a time.

uint8_t battlePage[32 * 16];
bool battleTried = false, battleOk = false;

uint8_t typeChart[128 * 3];
int typeChartRecs = 0;
bool chartTried = false;

uint8_t townTiles[16 * 16];
uint8_t townCells[20 * 18];
bool townTried = false, townOk = false;

// A tileset is at most 96 tiles and a blockset at most 128 blocks in this
// checkout; the caps below leave room for a pack built from a hack.
const int TILESET_MAX = 256;
const int BLOCKSET_MAX = 256;
uint8_t tilesetTiles[TILESET_MAX * 16];
uint8_t blocksetBlocks[BLOCKSET_MAX * 16];
int cachedTileset = -1, cachedTilesetCount = 0;
int cachedBlockset = -1, cachedBlockCount = 0;

// One Pokemon picture, front (up to 7x7 tiles) or back (4x4), one at a time.
uint8_t picBuf[7 * 7 * 16];

// ------------------------------------------------------------ little helpers

uint16_t le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

int clampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// One string out of a STRTAB section: u16 count, u16 offset per entry, then
// the zero terminated strings.
String packString(const char *section, int index, size_t cap = 32) {
  if (index < 0) return String();
  uint8_t head[2];
  if (pack::read(section, 0, head, 2) != 2) return String();
  int count = (int)le16(head);
  if (index >= count) return String();
  uint8_t off[2];
  if (pack::read(section, 2 + 2 * (size_t)index, off, 2) != 2) return String();
  char buf[40];
  if (cap > sizeof(buf) - 1) cap = sizeof(buf) - 1;
  size_t n = pack::read(section, le16(off), buf, cap);
  buf[n] = 0;
  return String(buf);
}

String rjust(long v, int cells) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%*ld", cells, v);
  return String(buf);
}

String ljust(const String &s, int cells) {
  String out = s;
  if ((int)out.length() > cells) out = out.substring(0, cells);
  while ((int)out.length() < cells) out += ' ';
  return out;
}

// ------------------------------------------------------------- pack lookups

const uint8_t *battleTile(uint8_t code) {
  if (!battleTried) {
    battleTried = true;
    battleOk = pack::read(S_FONT_BATTLE, 0, battlePage, sizeof(battlePage)) ==
               sizeof(battlePage);
  }
  if (!battleOk || code < 0x60) return nullptr;
  return battlePage + (size_t)(code - 0x60) * 16;
}

void drawBattleTile(uint8_t code, int x, int y, int scale) {
  const uint8_t *t = battleTile(code);
  if (t) gbgfx::drawTile2bpp(t, x, y, scale, true);
}

uint8_t dexOf(uint8_t internalIndex) {
  if (!internalIndex) return 0;
  uint8_t dex = 0;
  pack::read(S_DEX_ORDER, (size_t)internalIndex - 1, &dex, 1);
  return dex;
}

String speciesNameByDex(uint8_t dex) {
  if (!dex || dex > 151) return String();
  return packString(S_SPECIES_NAMES, dex - 1, 12);
}

String speciesName(uint8_t internalIndex) {
  return speciesNameByDex(dexOf(internalIndex));
}

bool baseStats(uint8_t dex, uint8_t out[9]) {
  if (!dex || dex > 151) return false;
  return pack::read(S_BASE_STATS, (size_t)(dex - 1) * 9, out, 9) == 9;
}

String typeName(uint8_t type) {
  return packString(S_TYPE_NAMES, type, 10);
}

String typeLine(uint8_t t1, uint8_t t2) {
  String a = typeName(t1);
  if (t2 == t1) return a;
  String b = typeName(t2);
  if (!b.length()) return a;
  return a + "/" + b;
}

// Multiplier times ten of one attacking type against one defending type.
int chartMul(uint8_t atk, uint8_t def) {
  if (!chartTried) {
    chartTried = true;
    size_t len = pack::size(S_TYPE_CHART);
    if (len > sizeof(typeChart)) len = sizeof(typeChart);
    size_t got = pack::read(S_TYPE_CHART, 0, typeChart, len);
    typeChartRecs = (int)(got / 3);
  }
  for (int i = 0; i < typeChartRecs; i++) {
    const uint8_t *r = typeChart + i * 3;
    if (r[0] == atk && r[1] == def) return r[2];
  }
  return 10;   // any pair the table does not list is neutral
}

// The product of both defending types, as a percentage of neutral.
int effPercent(uint8_t moveType, uint8_t d1, uint8_t d2) {
  // Both entries are ten times their multiplier, so the product is already a
  // percentage of neutral: 5 and 5 give 25, 20 and 20 give 400.
  int m = chartMul(moveType, d1);
  int n = (d2 != d1) ? chartMul(moveType, d2) : 10;
  return m * n;
}

// Three cells, inverted, or empty for neutral. 'x' is the game's own times
// glyph: gen1Encode maps it to code $F1.
String effBadge(int percent) {
  switch (percent) {
    case 0: return "x0 ";
    case 25: return "1/4";
    case 50: return "1/2";
    case 200: return "x2 ";
    case 400: return "x4 ";
    default: return String();
  }
}

String itemName(uint8_t id) { return packString(S_ITEM_NAMES, id, 14); }

uint16_t itemPrice(uint8_t id) {
  uint8_t p[2];
  if (pack::read(S_ITEM_PRICES, (size_t)id * 2, p, 2) != 2) return 0;
  return le16(p);
}

String moveName(uint8_t id) {
  if (!id) return String();
  return packString(S_MOVE_NAMES, id - 1, 14);
}

bool moveData(uint8_t id, uint8_t out[4]) {
  if (!id) return false;
  return pack::read(S_MOVE_DATA, (size_t)(id - 1) * 4, out, 4) == 4;
}

String mapLabel(uint8_t map) { return packString(S_MAP_LABELS, map, 30); }

String trainerClassName(uint8_t cls) {
  if (!cls) return String();
  return packString(S_TRAINER_CLASS, cls - 1, 14);
}

const char *statusText(uint8_t status, bool fainted) {
  if (fainted) return "FNT";
  if (status & 0x07) return "SLP";
  if (status & 0x08) return "PSN";
  if (status & 0x10) return "BRN";
  if (status & 0x20) return "FRZ";
  if (status & 0x40) return "PAR";
  return "";
}

// ---------------------------------------------------------------- tile draw

uint8_t rev8(uint8_t b) {
  b = (uint8_t)((b >> 4) | (b << 4));
  b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
  b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
  return b;
}

// Every shade one step lighter: 3 to 2, 2 to 1, 1 to 0. Bitwise on the two
// planes, which is why the unearned badge faces cost nothing to dim.
void dimTile(const uint8_t *src, uint8_t *dst) {
  for (int i = 0; i < 8; i++) {
    uint8_t lo = src[i * 2], hi = src[i * 2 + 1];
    dst[i * 2] = (uint8_t)(hi & ~lo);
    dst[i * 2 + 1] = (uint8_t)(hi & lo);
  }
}

void mirrorTile(const uint8_t *src, uint8_t *dst) {
  for (int i = 0; i < 16; i++) dst[i] = rev8(src[i]);
}

// A row major run of tiles as one image. Shade 0 is left alone, so a sprite
// sits over whatever is already on the panel.
void drawTiles(const uint8_t *data, int tilesW, int tilesH, int x, int y,
               int scale, bool mirror = false, bool dim = false) {
  if (!data) return;
  uint8_t work[16], work2[16];
  for (int ty = 0; ty < tilesH; ty++) {
    for (int tx = 0; tx < tilesW; tx++) {
      const uint8_t *t = data + (size_t)(ty * tilesW + tx) * 16;
      int dx = mirror ? (tilesW - 1 - tx) : tx;
      if (mirror) {
        mirrorTile(t, work);
        t = work;
      }
      if (dim) {
        dimTile(t, work2);
        t = work2;
      }
      gbgfx::drawTile2bpp(t, x + dx * 8 * scale, y + ty * 8 * scale, scale,
                          true);
    }
  }
}

// A Pokemon picture by dex number, from the front or back blob. Returns the
// side in tiles, 0 when the pack cannot supply it. The bytes land in picBuf,
// so only one picture exists at a time.
int loadPic(uint8_t dex, bool front) {
  if (!dex || dex > 151) return 0;
  uint8_t rec[5];
  const char *idx = front ? S_FRONT_INDEX : S_BACK_INDEX;
  const char *blob = front ? S_FRONT : S_BACK;
  if (pack::read(idx, (size_t)(dex - 1) * 5, rec, 5) != 5) return 0;
  uint32_t off = le32(rec);
  int side = rec[4];
  if (side < 1 || side > 7) return 0;
  size_t len = (size_t)side * side * 16;
  if (pack::read(blob, off, picBuf, len) != len) return 0;
  return side;
}

// ----------------------------------------------------------------- HP bar
//
// The game's nine tile bar: HP: glyph, left cap, six fill tiles, right cap.
// Fill is 48 source pixels wide; the colour rule repaints the fill band.

int hpFill(int hp, int maxHp) {
  if (maxHp <= 0 || hp <= 0) return 0;
  int e = 48 * hp / maxHp;
  if (e == 0) e = 1;
  if (e > 48) e = 48;
  return e;
}

// 0 green, 1 yellow, 2 red, exactly GetHealthBarColor.
int hpColor(int e) { return e >= 27 ? 0 : (e >= 10 ? 1 : 2); }

void drawHpBar(int x, int y, int scale, int hp, int maxHp, bool partyCap) {
  int e = hpFill(hp, maxHp);
  int step = 8 * scale;
  drawBattleTile(TILE_HP, x, y, scale);
  drawBattleTile(TILE_BAR_LEFT, x + step, y, scale);
  for (int i = 0; i < 6; i++) {
    int remaining = e - i * 8;
    uint8_t code;
    if (remaining >= 8) {
      code = TILE_BAR_FULL;
    } else if (remaining > 0) {
      code = (uint8_t)(TILE_BAR_EMPTY + remaining);
    } else {
      code = TILE_BAR_EMPTY;
    }
    drawBattleTile(code, x + step * (2 + i), y, scale);
  }
  drawBattleTile(partyCap ? TILE_BAR_CAP_PARTY : TILE_BAR_CAP_BATTLE,
                 x + step * 8, y, scale);

  // Repaint the 2 px fill band over the filled part: solid for green, the
  // ROM's own checker for yellow (so nothing to do), sparse for red.
  int colour = hpColor(e);
  if (!e || colour == 1) return;
  int bx = x + 2 * step;
  int by = y + 3 * scale;
  int bh = 2 * scale;
  int bw = e * scale;
  if (colour == 0) {
    gbgfx::fillRect(bx, by, bw, bh, ui::INK_BLACK);
    return;
  }
  gbgfx::fillSparse(bx, by, bw, bh);
}

// `123/123`, seven cells, the current value inverted when the bar is red.
void drawHpNumbers(int x, int y, int scale, int hp, int maxHp) {
  bool red = hpColor(hpFill(hp, maxHp)) == 2;
  String cur = rjust(hp, 3);
  gbgfx::printGb(x, y, cur, scale, red);
  gbgfx::printGb(x + 3 * 8 * scale, y, "/" + rjust(maxHp, 3), scale, false);
}

// --------------------------------------------------------------- the chrome

String playTimeText() {
  pokemon::PlayTime t = pokemon::playTime();
  char buf[20];
  snprintf(buf, sizeof(buf), "TIME %u:%02u", (unsigned)t.hours,
           (unsigned)t.minutes);
  return String(buf);
}

// The reader's own clock at the moment the last snapshot was accepted.
String updateText() {
  time_t now = time(nullptr);
  if (!pokemon::wramValid() || now < (time_t)NET_TIME_SANE_EPOCH) {
    return "UPD --:--:--";
  }
  uint32_t ageMs = millis() - pokemon::wramStamp();
  time_t at = now - (time_t)(ageMs / 1000) +
              (time_t)CLOCK_UTC_OFFSET_MINUTES * 60;
  struct tm parts;
  gmtime_r(&at, &parts);   // already shifted, so gmtime and not localtime
  char buf[20];
  snprintf(buf, sizeof(buf), "UPD %02d:%02d:%02d", parts.tm_hour, parts.tm_min,
           parts.tm_sec);
  return String(buf);
}

void drawTopBar(const char *viewName) {
  ui::gfx().fillRect(0, 0, SCREEN_W, gbgfx::mapY(TOP_BAR_H), ui::INK_BLACK);
  gbgfx::printGb(12, BAR_TEXT_Y, viewName, 2, true);
  gbgfx::printGb(172, BAR_TEXT_Y, ljust(pokemon::playerName(), 7), 2, true);
  gbgfx::printGb(292, BAR_TEXT_Y, playTimeText(), 2, true);
  gbgfx::printGb(476, BAR_TEXT_Y, updateText(), 2, true);
}

// The hint band sits at the bottom of the canvas, not of the scaled page: on
// the 7.5 inch panel the page is 649 px tall on an 800 px canvas, and the
// spare 151 px go between the page body and the band. On panel 0 the shift
// is 920 - 920 = 0.
void drawHint(const char *text) {
  gbgfx::setOriginY(SCREEN_H - gbgfx::layoutHeight());
  int top = gbgfx::mapY(HINT_Y);
  ui::gfx().fillRect(0, top, SCREEN_W, SCREEN_H - top, ui::INK_BLACK);
  gbgfx::printGb(12, HINT_TEXT_Y, text, 2, true);
  gbgfx::setOriginY(0);
}

// ------------------------------------------------------------- view: home

void drawTownMap() {
  if (!townTried) {
    townTried = true;
    townOk = pack::read(S_TOWN_TILES, 0, townTiles, sizeof(townTiles)) ==
                 sizeof(townTiles) &&
             pack::read(S_TOWN_MAP, 0, townCells, sizeof(townCells)) ==
                 sizeof(townCells);
  }
  if (!townOk) return;
  for (int cy = 0; cy < 18; cy++) {
    for (int cx = 0; cx < 20; cx++) {
      uint8_t t = townCells[cy * 20 + cx] & 0x0F;
      gbgfx::drawTile2bpp(townTiles + (size_t)t * 16, 180 + 16 * cx,
                          32 + 16 * cy, 2, true);
    }
  }

  // The player's cell, from the town map entry for the current map (an indoor
  // map already carries its outdoor parent's cell in the pack).
  uint8_t entry[3];
  if (pack::read(S_TOWN_ENTRIES, (size_t)pokemon::curMap() * 3, entry, 3) != 3) {
    return;
  }
  int cx = entry[0] % 16, cy = entry[1] % 16;

  // The game puts the 16x16 cursor sprite at OAM (x*8+24, y*8+24), which is
  // screen pixel (x*8+16, y*8+8) after the OAM offset (TownMapCoordsToOAMCoords
  // in pokered engine/items/town_map.asm). At 2x that is a 32x32 block.
  // On glass the sprite sat half a cell right and below the town, so the
  // block is pulled back by 8 px on both axes (founder aligned 2026-09-12).
  int sx = 180 + 16 * cx + 24, sy = 32 + 16 * cy + 8;

  // A sparse 1 px box 4 px outside the sprite, then the game's cursor sprite.
  int bx = sx - 4, by = sy - 4;
  for (int i = 0; i < 40; i += 2) {
    gbgfx::drawPixel(bx + i, by, ui::INK_BLACK);
    gbgfx::drawPixel(bx + i, by + 39, ui::INK_BLACK);
    gbgfx::drawPixel(bx, by + i, ui::INK_BLACK);
    gbgfx::drawPixel(bx + 39, by + i, ui::INK_BLACK);
  }
  uint8_t cursor[4 * 16];
  if (pack::read(S_TOWN_CURSOR, 0, cursor, sizeof(cursor)) == sizeof(cursor)) {
    drawTiles(cursor, 2, 2, sx, sy, 2);
  }

  // Location box under the map.
  gbgfx::drawBox(180, 328, 20, 3, 2);
  String name = packString(S_MAP_NAMES, entry[2], 18);
  if (!name.length()) name = mapLabel(pokemon::curMap());
  gbgfx::printGb(196, 344, ljust(name, 18), 2, false);
}

void drawPartyIcon(uint8_t internalIndex, int x, int y, int scale) {
  uint8_t dex = dexOf(internalIndex);
  if (!dex || dex > 151) return;
  uint8_t iconId = 0;
  if (pack::read(S_ICON_MAP, (size_t)dex - 1, &iconId, 1) != 1) return;
  uint8_t head[4];
  if (pack::read(S_ICONS, 0, head, 4) != 4) return;
  int count = (int)le16(head);
  int tiles = (int)le16(head + 2);
  if (iconId >= count || tiles != 4) return;
  uint8_t icon[4 * 16];
  if (pack::read(S_ICONS, 4 + (size_t)iconId * 64, icon, sizeof(icon)) !=
      sizeof(icon)) {
    return;
  }
  drawTiles(icon, 2, 2, x, y, scale);
}

void drawTrainerCards() {
  // Left card: name, id, money, coins, rival.
  gbgfx::drawBox(4, 32, 10, 21, 2);
  gbgfx::printGb(20, 48, "NAME", 2, false);
  gbgfx::printGb(20, 64, " " + pokemon::playerName(), 2, false);
  drawBattleTile(TILE_ID, 20, 96, 2);
  drawBattleTile(TILE_NO, 36, 96, 2);
  gbgfx::printGb(20, 112, " " + rjust(pokemon::playerId(), 5), 2, false);
  gbgfx::printGb(20, 144, "MONEY", 2, false);
  {
    char buf[12];
    snprintf(buf, sizeof(buf), "$%6lu", (unsigned long)pokemon::money());
    gbgfx::printGb(20, 160, buf, 2, false);
  }
  gbgfx::printGb(20, 192, "COINS", 2, false);
  gbgfx::printGb(20, 208, rjust(pokemon::coins(), 5), 2, false);
  gbgfx::printGb(20, 240, "RIVAL", 2, false);
  gbgfx::printGb(20, 256, " " + pokemon::rivalName(), 2, false);

  // Right card: dex, play time, repel, map, bag.
  gbgfx::drawBox(516, 32, 10, 21, 2);
  int px = gbgfx::printGb(532, 48, "POK", 2, false);
  gbgfx::drawFontCode(CODE_E_ACUTE, px, 48, 2, false);
  gbgfx::printGb(px + 16, 48, "DEX", 2, false);
  gbgfx::printGb(532, 64, "OWN " + rjust(pokemon::dexOwned(), 3), 2, false);
  gbgfx::printGb(532, 80, "SEEN " + rjust(pokemon::dexSeen(), 3), 2, false);
  gbgfx::printGb(532, 112, "TIME", 2, false);
  {
    pokemon::PlayTime t = pokemon::playTime();
    char buf[16];
    snprintf(buf, sizeof(buf), " %u:%02u", (unsigned)t.hours,
             (unsigned)t.minutes);
    gbgfx::printGb(532, 128, buf, 2, false);
  }
  gbgfx::printGb(532, 160, "REPEL", 2, false);
  {
    uint8_t steps = pokemon::repelSteps();
    gbgfx::printGb(532, 176, steps ? (" " + String((int)steps)) : String(" none"),
                   2, false);
  }
  gbgfx::printGb(532, 208, "MAP " + String((int)pokemon::curMap()), 2, false);
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "X%02u Y%02u", (unsigned)pokemon::playerX(),
             (unsigned)pokemon::playerY());
    gbgfx::printGb(532, 224, buf, 2, false);
  }
  gbgfx::printGb(532, 256, "BAG", 2, false);
  gbgfx::printGb(532, 272, " " + String((int)pokemon::bagCount()) + "/20", 2,
                 false);
}

void drawPartyBox() {
  gbgfx::drawBox(4, 384, 42, 26, 2);
  int count = (int)pokemon::partyCount();
  for (int r = 0; r < count && r < 6; r++) {
    Mon m;
    if (!pokemon::partyMon(r, m)) continue;
    int y = 400 + 64 * r;
    drawPartyIcon(m.species, 20, y, 3);
    gbgfx::printGb(76, y, ljust(String(m.nick), 10), 3, false);
    drawBattleTile(TILE_LV, 340, y, 3);
    gbgfx::printGb(364, y, rjust(m.level, 3), 3, false);
    const char *st = statusText(m.status, m.hp == 0);
    if (*st) gbgfx::printGb(460, y, st, 3, false);

    drawHpBar(76, y + 24, 2, m.hp, m.maxHp, true);
    drawHpNumbers(228, y + 24, 2, m.hp, m.maxHp);
    gbgfx::printGb(356, y + 24, typeLine(m.type1, m.type2), 2, false);

    String detail = ljust(speciesName(m.species), 10) + "  OT " +
                    ljust(pokemon::otName(r), 7) + " " +
                    rjust(pokemon::otId(r), 5);
    gbgfx::printGb(76, y + 40, detail, 2, false);
  }
}

void drawBadgeBox() {
  gbgfx::drawBox(4, 808, 42, 5, 2);
  uint8_t owned = pokemon::badges();
  uint8_t numbers[8 * 16];
  bool haveNumbers =
      pack::read(S_BADGE_NUMBERS, 0, numbers, sizeof(numbers)) ==
      sizeof(numbers);
  for (int k = 0; k < 8; k++) {
    if (haveNumbers) {
      gbgfx::drawTile2bpp(numbers + (size_t)k * 16, 28 + 80 * k, 824, 2, true);
    }
    bool earned = (owned >> k) & 1;
    uint8_t art[4 * 16];
    const char *section = earned ? S_BADGES : S_LEADER_FACES;
    if (pack::read(section, (size_t)k * 64, art, sizeof(art)) != sizeof(art)) {
      continue;
    }
    drawTiles(art, 2, 2, 44 + 80 * k, 824, 3, false, !earned);
  }
}

void viewHome() {
  drawTopBar("HOME");
  drawTownMap();
  drawTrainerCards();
  drawPartyBox();
  drawBadgeBox();
  drawHint("R:INVENTORY  L:BACK  U:TERRAIN  C:EXIT");
}

// -------------------------------------------------------- view: inventory

void viewInventory() {
  drawTopBar("INVENTORY");

  gbgfx::drawBox(4, 32, 28, 22, 3);
  int bag = (int)pokemon::bagCount();
  for (int i = 0; i < bag && i < 20; i++) {
    uint8_t id = 0, qty = 0;
    if (!pokemon::bagItem(i, id, qty)) break;
    int y = 56 + 24 * i;
    gbgfx::printGb(52, y, ljust(itemName(id), 13), 3, false);
    gbgfx::printGb(388, y, "x" + rjust(qty, 2), 3, false);
    gbgfx::printGb(508, y, "$" + rjust(itemPrice(id), 5), 3, false);
  }

  // PC items, left.
  gbgfx::drawBox(4, 568, 21, 20, 2);
  int pc = (int)pokemon::pcItemCount();
  gbgfx::printGb(20, 584, "PC ITEMS " + String(pc) + "/50", 2, false);
  for (int j = 0; j < 16 && j < pc; j++) {
    uint8_t id = 0, qty = 0;
    if (!pokemon::pcItem(j, id, qty)) break;
    gbgfx::printGb(20, 600 + 16 * j, ljust(itemName(id), 12) + " x" +
                                         rjust(qty, 2),
                   2, false);
  }
  if (pc > 17) {
    gbgfx::printGb(20, 856, ".." + String(pc - 16) + " MORE", 2, false);
  } else if (pc == 17) {
    uint8_t id = 0, qty = 0;
    if (pokemon::pcItem(16, id, qty)) {
      gbgfx::printGb(20, 856, ljust(itemName(id), 12) + " x" + rjust(qty, 2), 2,
                     false);
    }
  }

  // Money, coins, box and day care, right.
  gbgfx::drawBox(340, 568, 21, 20, 2);
  {
    char buf[24];
    snprintf(buf, sizeof(buf), "MONEY  $%lu", (unsigned long)pokemon::money());
    gbgfx::printGb(356, 584, buf, 2, false);
  }
  gbgfx::printGb(356, 600, "COINS " + rjust(pokemon::coins(), 7), 2, false);

  int boxNum = (int)pokemon::boxNumber();
  int inBox = (int)pokemon::boxCount();
  gbgfx::printGb(356, 632,
                 "BOX " + String(boxNum) + "  " + String(inBox) + "/20", 2,
                 false);
  gbgfx::printGb(356, 648, "DAY CARE", 2, false);
  if (pokemon::dayCareInUse()) {
    int x = gbgfx::printGb(356, 664, " " + ljust(pokemon::dayCareName(), 10) +
                                         "  ",
                           2, false);
    drawBattleTile(TILE_LV, x, 664, 2);
    gbgfx::printGb(x + 16, 664, rjust(pokemon::dayCareLevel(), 3), 2, false);
  } else {
    gbgfx::printGb(356, 664, " EMPTY", 2, false);
  }

  gbgfx::printGb(356, 696, "IN BOX " + String(boxNum), 2, false);
  int shown = inBox < 9 ? inBox : 9;
  for (int j = 0; j < shown; j++) {
    int x = gbgfx::printGb(356, 712 + 16 * j,
                           " " + ljust(speciesName(pokemon::boxMonSpecies(j)),
                                       10) +
                               "  ",
                           2, false);
    drawBattleTile(TILE_LV, x, 712 + 16 * j, 2);
    gbgfx::printGb(x + 16, 712 + 16 * j, rjust(pokemon::boxMonLevel(j), 3), 2,
                   false);
  }
  if (inBox > 10) {
    gbgfx::printGb(356, 856, ".." + String(inBox - 9) + " MORE", 2, false);
  } else if (inBox == 10) {
    int x = gbgfx::printGb(356, 856,
                           " " + ljust(speciesName(pokemon::boxMonSpecies(9)),
                                       10) +
                               "  ",
                           2, false);
    drawBattleTile(TILE_LV, x, 856, 2);
    gbgfx::printGb(x + 16, 856, rjust(pokemon::boxMonLevel(9), 3), 2, false);
  }

  drawHint("R:HOME  L:BACK  U:TERRAIN  C:EXIT");
}

// ---------------------------------------------------------- view: terrain

struct MapInfo {
  uint32_t offset;
  uint8_t w, h, tileset, border;
  bool ok;
};

MapInfo mapInfo(uint8_t map) {
  MapInfo mi;
  mi.ok = false;
  mi.offset = 0;
  mi.w = mi.h = mi.tileset = mi.border = 0;
  uint8_t rec[8];
  if (pack::read(S_MAP_INDEX, (size_t)map * 8, rec, 8) != 8) return mi;
  mi.offset = le32(rec);
  mi.w = rec[4];
  mi.h = rec[5];
  mi.tileset = rec[6];
  mi.border = rec[7];
  mi.ok = mi.w > 0 && mi.h > 0;
  return mi;
}

// Tileset graphics and blockset for one tileset id, cached so a terrain
// redraw does not reread 4 KB from the filesystem every time.
bool loadTileset(uint8_t tileset) {
  if (cachedTileset == (int)tileset) return cachedTilesetCount > 0;
  cachedTileset = (int)tileset;
  cachedTilesetCount = 0;
  uint8_t rec[6];
  if (pack::read(S_TILESET_INDEX, (size_t)tileset * 6, rec, 6) != 6) return false;
  int count = (int)le16(rec + 4);
  if (count < 1) return false;
  if (count > TILESET_MAX) count = TILESET_MAX;
  size_t len = (size_t)count * 16;
  if (pack::read(S_TILESETS, le32(rec), tilesetTiles, len) != len) return false;
  cachedTilesetCount = count;
  return true;
}

bool loadBlockset(uint8_t tileset) {
  if (cachedBlockset == (int)tileset) return cachedBlockCount > 0;
  cachedBlockset = (int)tileset;
  cachedBlockCount = 0;
  uint8_t rec[6];
  if (pack::read(S_BLOCKSET_INDEX, (size_t)tileset * 6, rec, 6) != 6) return false;
  int count = (int)le16(rec + 4);
  if (count < 1) return false;
  if (count > BLOCKSET_MAX) count = BLOCKSET_MAX;
  size_t len = (size_t)count * 16;
  if (pack::read(S_BLOCKSETS, le32(rec), blocksetBlocks, len) != len) return false;
  cachedBlockCount = count;
  return true;
}

// One 4x4 tile block at 2x, 64x64 px.
void drawBlock(uint8_t block, int x, int y) {
  if (block >= cachedBlockCount) return;
  const uint8_t *b = blocksetBlocks + (size_t)block * 16;
  for (int ty = 0; ty < 4; ty++) {
    for (int tx = 0; tx < 4; tx++) {
      uint8_t t = b[ty * 4 + tx];
      if (t >= cachedTilesetCount) continue;
      gbgfx::drawTile2bpp(tilesetTiles + (size_t)t * 16, x + tx * 16,
                          y + ty * 16, 2, true);
    }
  }
}

// A merged wild encounter table: at most ten species with summed odds and a
// level range.
struct Encounter {
  uint8_t species;
  uint8_t levelLo, levelHi;
  int odds;
};

const int SLOT_ODDS[10] = {20, 20, 15, 10, 10, 10, 5, 5, 4, 1};

int mergeSlots(const uint8_t *slots, Encounter *out) {
  int n = 0;
  for (int i = 0; i < 10; i++) {
    uint8_t level = slots[i * 2];
    uint8_t species = slots[i * 2 + 1];
    if (!species) continue;
    int found = -1;
    for (int j = 0; j < n; j++) {
      if (out[j].species == species) {
        found = j;
        break;
      }
    }
    if (found < 0) {
      out[n].species = species;
      out[n].levelLo = level;
      out[n].levelHi = level;
      out[n].odds = SLOT_ODDS[i];
      n++;
    } else {
      if (level < out[found].levelLo) out[found].levelLo = level;
      if (level > out[found].levelHi) out[found].levelHi = level;
      out[found].odds += SLOT_ODDS[i];
    }
  }
  return n;
}

// `CATERPIE   L  3  40`: name 10, space, L, a three cell level field that can
// hold a range, space, odds 2. Eighteen cells.
String encounterRow(const Encounter &e) {
  String level;
  if (e.levelLo == e.levelHi) {
    level = rjust(e.levelLo, 3);
  } else {
    level = String((int)e.levelLo) + "-" + String((int)e.levelHi);
    while (level.length() < 3) level += ' ';
    if (level.length() > 3) level = level.substring(0, 3);
  }
  return ljust(speciesName(e.species), 10) + " L" + level + " " +
         rjust(e.odds, 2);
}

struct MapItem {
  uint8_t x, y, item;
  bool hidden;
  bool taken;
};

// Placed item balls and hidden ground items on one map, in pack order.
int collectItems(uint8_t map, MapItem *out, int cap) {
  int n = 0;
  uint8_t rec[5];
  size_t count = pack::size(S_ITEMS_PLACED) / 5;
  for (size_t i = 0; i < count && n < cap; i++) {
    if (pack::read(S_ITEMS_PLACED, i * 5, rec, 5) != 5) break;
    if (rec[0] != map) continue;
    out[n].x = rec[1];
    out[n].y = rec[2];
    out[n].item = rec[3];
    out[n].hidden = false;
    out[n].taken = pokemon::objectTaken(rec[4]);
    n++;
  }
  count = pack::size(S_ITEMS_HIDDEN) / 5;
  for (size_t i = 0; i < count && n < cap; i++) {
    if (pack::read(S_ITEMS_HIDDEN, i * 5, rec, 5) != 5) break;
    if (rec[0] != map) continue;
    out[n].x = rec[1];
    out[n].y = rec[2];
    out[n].item = rec[3];
    out[n].hidden = true;
    out[n].taken = pokemon::hiddenItemTaken(rec[4]);
    n++;
  }
  return n;
}

void drawItemRow(int x, int y, const MapItem &it, int nameCells) {
  char coords[8];
  snprintf(coords, sizeof(coords), "%02u,%02u", (unsigned)it.x, (unsigned)it.y);
  String row = String(it.hidden ? "H" : " ") + ljust(itemName(it.item), nameCells) +
               " " + coords;
  gbgfx::printGb(x, y, row, 2, false);
  if (it.taken) {
    int cells = 1 + nameCells + 1 + 5;
    gbgfx::fillRect(x + 16, y + 7, (cells - 1) * 16, 2, ui::INK_BLACK);
  }
}

void viewTerrain() {
  drawTopBar("TERRAIN");
  uint8_t map = pokemon::curMap();
  MapInfo mi = mapInfo(map);

  uint8_t wild[42];
  bool haveWild = pack::read(S_WILD, (size_t)map * 42, wild, 42) == 42 &&
                  (wild[0] || wild[21]);
  MapItem items[40];
  int itemCount = collectItems(map, items, 40);
  bool haveItems = itemCount > 0;
  bool tables = haveWild || haveItems;

  const int winCols = 10;
  const int winRows = tables ? 8 : 13;

  int px = pokemon::playerX(), py = pokemon::playerY();
  int pbx = px / 2, pby = py / 2;
  int bx = 0, by = 0, colOffset = 0, rowOffset = 0;
  if (mi.ok) {
    if (mi.w >= winCols) {
      bx = clampInt(pbx - winCols / 2, 0, mi.w - winCols);
    } else {
      colOffset = (winCols - mi.w) / 2;
    }
    if (mi.h >= winRows) {
      by = clampInt(pby - winRows / 2, 0, mi.h - winRows);
    } else {
      rowOffset = (winRows - mi.h) / 2;
    }
  }

  // Caption. The right hand fields drop in order WIN, BLK when the line runs
  // past 41 cells.
  {
    char tail[64];
    String head = mapLabel(map);
    if (!head.length()) head = "MAP";
    char pos[24];
    snprintf(pos, sizeof(pos), "  MAP %u  X%02u Y%02u", (unsigned)map,
             (unsigned)px, (unsigned)py);
    String line = head + pos;
    if (mi.ok) {
      snprintf(tail, sizeof(tail), "  BLK %ux%u", (unsigned)mi.w,
               (unsigned)mi.h);
      String withBlk = line + tail;
      char win[24];
      snprintf(win, sizeof(win), "  WIN %d,%d", bx, by);
      String withWin = withBlk + win;
      bool bigger = mi.w > winCols || mi.h > winRows;
      if (bigger && withWin.length() <= 41) {
        line = withWin;
      } else if (withBlk.length() <= 41) {
        line = withBlk;
      }
    }
    gbgfx::printGb(12, 24, line.substring(0, 41), 2, false);
  }

  // The map window. Blocks outside the map are the map's own border block,
  // which is what the game fills its screen edge with.
  if (mi.ok && loadTileset(mi.tileset) && loadBlockset(mi.tileset)) {
    for (int j = 0; j < winRows; j++) {
      esp_task_wdt_reset();
      int my = by + j - rowOffset;
      uint8_t row[16];
      bool rowOk = false;
      if (my >= 0 && my < mi.h) {
        int take = mi.w - bx;
        if (take > winCols) take = winCols;
        if (take > 0) {
          rowOk = pack::read(S_MAPS, mi.offset + (size_t)my * mi.w + bx, row,
                             (size_t)take) == (size_t)take;
          for (int i = take; i < winCols; i++) row[i] = mi.border;
        }
      }
      for (int i = 0; i < winCols; i++) {
        int mx = bx + i - colOffset;
        uint8_t block = mi.border;
        if (rowOk && mx >= 0 && mx < mi.w && i - colOffset >= 0 &&
            i - colOffset < winCols) {
          block = row[i - colOffset];
        }
        drawBlock(block, 20 + 64 * i, 40 + 64 * j);
      }
    }

    // Red, from the three packed frames; right facing is the left frame
    // mirrored, the way the game draws it.
    uint8_t walk[3 * 4 * 16];
    if (pack::read(S_PLAYER_WALK, 0, walk, sizeof(walk)) == sizeof(walk)) {
      uint8_t facing = pokemon::playerFacing();
      int frame = 0;
      bool mirror = false;
      if (facing == 4) {
        frame = 1;
      } else if (facing == 8) {
        frame = 2;
      } else if (facing == 12) {
        frame = 2;
        mirror = true;
      }
      int sx = 20 + 32 * (px - 2 * bx) + 64 * colOffset;
      int sy = 40 + 32 * (py - 2 * by) + 64 * rowOffset - 8;
      if (sx >= 20 && sx + 32 <= 660 && sy >= 32 && sy + 32 <= 40 + 64 * winRows) {
        drawTiles(walk + (size_t)frame * 64, 2, 2, sx, sy, 2, mirror);
      }
    }
  }

  if (tables) {
    // Two boxes, or one full width box when only one table exists.
    int encX = 4, encTiles = 20, encInner = 20;
    int itemX = 324, itemTiles = 22, itemInner = 340;
    if (haveWild && !haveItems) {
      encX = 4;
      encTiles = 42;
      encInner = 20;
    } else if (haveItems && !haveWild) {
      itemX = 4;
      itemTiles = 42;
      itemInner = 20;
    }

    if (haveWild) {
      gbgfx::drawBox(encX, 568, encTiles, 20, 2);
      Encounter grass[10], water[10];
      int ng = wild[0] ? mergeSlots(wild + 1, grass) : 0;
      int nw = wild[21] ? mergeSlots(wild + 22, water) : 0;
      bool twoColumns = encTiles == 42;
      int row = 0;
      int col2 = encInner + 320;

      gbgfx::printGb(encInner, 584,
                     "GRASS " + rjust((wild[0] * 100 + 128) / 256, 3) + " PCT", 2,
                     false);
      for (int i = 0; i < ng && row < 16; i++) {
        row++;
        gbgfx::printGb(encInner, 584 + 16 * row, encounterRow(grass[i]), 2,
                       false);
      }
      if (nw) {
        if (twoColumns) {
          gbgfx::printGb(col2, 584,
                         "WATER " + rjust((wild[21] * 100 + 128) / 256, 3) + " PCT", 2,
                         false);
          for (int i = 0; i < nw && i < 17; i++) {
            gbgfx::printGb(col2, 600 + 16 * i, encounterRow(water[i]), 2, false);
          }
        } else {
          row += 2;
          if (row < 17) {
            gbgfx::printGb(encInner, 584 + 16 * row,
                           "WATER " + rjust((wild[21] * 100 + 128) / 256, 3) + " PCT",
                           2, false);
            int dropped = 0;
            for (int i = 0; i < nw; i++) {
              if (row + 1 > 16) {
                dropped = nw - i;
                break;
              }
              row++;
              gbgfx::printGb(encInner, 584 + 16 * row, encounterRow(water[i]),
                             2, false);
            }
            if (dropped) {
              gbgfx::printGb(encInner, 856, ".." + String(dropped) + " MORE", 2,
                             false);
            }
          }
        }
      }
    }

    if (haveItems) {
      gbgfx::drawBox(itemX, 568, itemTiles, 20, 2);
      int taken = 0;
      for (int i = 0; i < itemCount; i++) {
        if (items[i].taken) taken++;
      }
      gbgfx::printGb(itemInner, 584,
                     "ITEMS  " + String(taken) + "/" + String(itemCount), 2,
                     false);
      // Rows 1 to 17 of the box, in one column or two. The last slot becomes
      // a ".. n MORE" line when the list does not fit.
      const int perColumn = 17;
      bool twoColumns = itemTiles == 42;
      int capacity = twoColumns ? 2 * perColumn : perColumn;
      int shown = itemCount <= capacity ? itemCount : capacity - 1;
      for (int i = 0; i < shown; i++) {
        int column = i / perColumn;
        int row = i % perColumn;
        drawItemRow(itemInner + (column ? 320 : 0), 600 + 16 * row, items[i],
                    13);
      }
      if (shown < itemCount) {
        int column = shown / perColumn;
        int row = shown % perColumn;
        gbgfx::printGb(itemInner + (column ? 320 : 0), 600 + 16 * row,
                       ".." + String(itemCount - shown) + " MORE", 2, false);
      }
    }
  }

  drawHint("R:NEXT PAGE  L:BACK  U:HOME  C:EXIT");
}

// ----------------------------------------------------------- view: battle

// The three cell inverted effectiveness badge, or nothing for neutral and for
// a status move.
void drawEffBadge(int x, int y, int scale, uint8_t moveType, uint8_t power,
                  uint8_t d1, uint8_t d2) {
  if (!power) return;
  String badge = effBadge(effPercent(moveType, d1, d2));
  if (!badge.length()) return;
  gbgfx::printGb(x, y, badge, scale, true);
}

void drawPartyStrip() {
  gbgfx::drawBox(4, 600, 42, 11, 2);
  int count = (int)pokemon::partyCount();
  int active = (int)pokemon::partyMonNumber();
  for (int p = 0; p < count && p < 6; p++) {
    Mon m;
    if (!pokemon::partyMon(p, m)) continue;
    int r = p / 2, column = p % 2;
    int cx = column ? 340 : 20;
    int y = 616 + 48 * r;
    drawPartyIcon(m.species, cx, y, 2);
    gbgfx::printGb(cx + 40, y, ljust(String(m.nick), 10), 2, p == active);
    drawBattleTile(TILE_LV, cx + 200, y, 2);
    gbgfx::printGb(cx + 216, y, rjust(m.level, 3), 2, false);
    const char *st = statusText(m.status, m.hp == 0);
    if (*st) gbgfx::printGb(cx + 272, y, st, 2, false);
    drawHpBar(cx + 40, y + 16, 2, m.hp, m.maxHp, true);
    drawHpNumbers(cx + 192, y + 16, 2, m.hp, m.maxHp);
  }
}

void viewBattle() {
  drawTopBar("BATTLE");
  BattleMon enemy, mine;
  bool haveEnemy = pokemon::enemyBattleMon(enemy);
  bool haveMine = pokemon::playerBattleMon(mine);
  bool trainer = pokemon::inBattle() == 2;

  if (haveEnemy) {
    uint8_t dex = dexOf(enemy.species);
    int side = loadPic(dex, true);
    if (side) {
      int w = side * 8 * 3;
      drawTiles(picBuf, side, side, 492 + (168 - w) / 2, 208 - w, 3);
    }
    gbgfx::printGb(20, 40, ljust(String(enemy.nick), 10), 3, false);
    drawBattleTile(TILE_LV, 284, 40, 3);
    gbgfx::printGb(308, 40, rjust(enemy.level, 3), 3, false);
    const char *st = statusText(enemy.status, enemy.hp == 0);
    if (*st) gbgfx::printGb(404, 40, st, 3, false);

    drawHpBar(20, 72, 3, enemy.hp, enemy.maxHp, false);
    drawHpNumbers(252, 76, 2, enemy.hp, enemy.maxHp);
    gbgfx::printGb(20, 104, typeLine(enemy.type1, enemy.type2), 2, false);
    if (!trainer) {
      gbgfx::printGb(276, 104, "CATCH " + rjust(enemy.catchRate, 3), 2, false);
    }

    if (trainer) {
      String line = "TRAINER " + ljust(trainerClassName(pokemon::trainerClass()),
                                       12) +
                    " " + String(pokemon::enemyAliveCount()) + "/" +
                    String((int)pokemon::enemyPartyCount()) + " LEFT";
      gbgfx::printGb(20, 128, line, 2, false);
    } else {
      int x = 20;
      drawBattleTile(TILE_NO, x, 128, 2);
      const char *flag = "NEW";
      if (pokemon::dexOwnedBit(dex)) {
        flag = "OWNED";
      } else if (pokemon::dexSeenBit(dex)) {
        flag = "SEEN";
      }
      gbgfx::printGb(x + 16, 128, rjust(dex, 3) + "  " + flag, 2, false);
    }

    uint8_t bs[9];
    if (baseStats(dex, bs)) {
      char line[48];
      snprintf(line, sizeof(line), "HP %u ATK %u DEF %u", (unsigned)bs[0],
               (unsigned)bs[1], (unsigned)bs[2]);
      gbgfx::printGb(20, 152, line, 2, false);
      snprintf(line, sizeof(line), "SPD %u SPC %u", (unsigned)bs[3],
               (unsigned)bs[4]);
      gbgfx::printGb(20, 176, line, 2, false);
    }
  }

  // Divider: the box rule tile across the whole width.
  for (int i = 0; i < 42; i++) {
    gbgfx::drawFontCode(CODE_RULE, 4 + 16 * i, 216, 2, false);
  }

  if (haveMine) {
    int side = loadPic(dexOf(mine.species), false);
    if (side) drawTiles(picBuf, side, side, 20, 248, 3);
    gbgfx::printGb(132, 240, ljust(String(mine.nick), 10), 3, false);
    drawBattleTile(TILE_LV, 396, 240, 3);
    gbgfx::printGb(420, 240, rjust(mine.level, 3), 3, false);
    const char *st = statusText(mine.status, mine.hp == 0);
    if (*st) gbgfx::printGb(516, 240, st, 3, false);

    drawHpBar(132, 272, 3, mine.hp, mine.maxHp, false);
    drawHpNumbers(364, 272, 3, mine.hp, mine.maxHp);
    gbgfx::printGb(132, 304, typeLine(mine.type1, mine.type2), 2, false);
    gbgfx::printGb(388, 304, speciesName(mine.species), 2, false);
    {
      char line[48];
      snprintf(line, sizeof(line), "ATK %u DEF %u SPD %u SPC %u",
               (unsigned)mine.atk, (unsigned)mine.def, (unsigned)mine.spd,
               (unsigned)mine.spc);
      gbgfx::printGb(132, 328, line, 2, false);
    }

    // Four moves.
    gbgfx::drawBox(4, 360, 28, 10, 3);
    for (int m = 0; m < 4; m++) {
      uint8_t id = mine.moves[m];
      if (!id) continue;
      uint8_t md[4];
      if (!moveData(id, md)) continue;
      int y = 384 + 48 * m;
      gbgfx::printGb(28, y, ljust(moveName(id), 12), 3, false);
      gbgfx::printGb(340, y, rjust(mine.pp[m], 2) + "/" + rjust(md[3], 2), 3,
                     false);
      if (haveEnemy) {
        drawEffBadge(580, y, 3, md[0], md[1], enemy.type1, enemy.type2);
      }
      String detail = ljust(typeName(md[0]), 8) + "  POW " + rjust(md[1], 3) +
                      "  ACC" + rjust(md[2], 3);
      if (md[0] == mine.type1 || md[0] == mine.type2) detail += "  STAB";
      gbgfx::printGb(28, y + 24, detail, 2, false);
    }
  }

  drawPartyStrip();

  // The enemy's own moves against our active mon.
  gbgfx::drawBox(4, 776, 42, 7, 2);
  gbgfx::printGb(20, 792, "ENEMY MOVES", 2, false);
  if (haveEnemy) {
    for (int j = 0; j < 4; j++) {
      uint8_t id = enemy.moves[j];
      if (!id) continue;
      uint8_t md[4];
      if (!moveData(id, md)) continue;
      int y = 808 + 16 * j;
      gbgfx::printGb(20, y, ljust(moveName(id), 12), 2, false);
      gbgfx::printGb(228, y, ljust(typeName(md[0]), 8), 2, false);
      gbgfx::printGb(372, y, "PP " + rjust(enemy.pp[j], 2) + "/" +
                                 rjust(md[3], 2),
                     2, false);
      gbgfx::printGb(516, y, "POW " + rjust(md[1], 3), 2, false);
      if (haveMine) {
        drawEffBadge(612, y, 2, md[0], md[1], mine.type1, mine.type2);
      }
    }
  }

  drawHint("R:NEXT PAGE  L:BACK  U:TERRAIN  C:EXIT");
}

// ----------------------------------------------------------------- no pack

void viewNoPack() {
  ui::gfx().fillScreen(ui::INK_WHITE);
  gbgfx::printGb(TEXT_MARGIN_X, 420,
                 "pokered.pack missing, upload it on the games page", 2, false);
  ui::drawStatusStrip("pokemon", "center exits");
}

// ------------------------------------------------------------------ picker

enum class View : uint8_t { NONE, HOME, INVENTORY, TERRAIN, BATTLE, NO_PACK };

View lastView = View::NONE;
uint32_t lastFullMs = 0;

View pickView(int page, bool terrain) {
  if (!pack::isOpen()) return View::NO_PACK;
  // A battle takes the screen whatever the pads say, and the view that was up
  // comes back by itself when the fight ends.
  if (pokemon::inBattle()) return View::BATTLE;
  if (terrain) return View::TERRAIN;
  return (page % pokemon_views::PAGE_COUNT) ? View::INVENTORY : View::HOME;
}

}  // namespace

namespace pokemon_views {

void reset() {
  battleTried = battleOk = false;
  chartTried = false;
  typeChartRecs = 0;
  townTried = townOk = false;
  cachedTileset = cachedBlockset = -1;
  cachedTilesetCount = cachedBlockCount = 0;
  lastView = View::NONE;
}

void draw(int page, bool terrain) {
  View v = pickView(page, terrain);
  // Full refresh on a view change and at least every GAMES_AUX_FULL_MS,
  // partial in between so a battle HP change lands within seconds.
  uint32_t now = millis();
  bool full = (v != lastView) || (now - lastFullMs >= GAMES_AUX_FULL_MS);
  lastView = v;
  if (full) lastFullMs = now;

  gbgfx::setLayout(DESIGN_W, DESIGN_H);
  pack::statReset();
  uint32_t renderMs = 0, pageMs = 0;
  ui::frameBegin(full);
  do {
    uint32_t t0 = millis();
    ui::gfx().fillScreen(ui::INK_WHITE);
    switch (v) {
      case View::HOME: viewHome(); break;
      case View::INVENTORY: viewInventory(); break;
      case View::TERRAIN: viewTerrain(); break;
      case View::BATTLE: viewBattle(); break;
      default: viewNoPack(); break;
    }
    uint32_t t1 = millis();
    renderMs += t1 - t0;
    bool more = ui::frameNext();
    pageMs += millis() - t1;
    if (!more) break;
  } while (true);
  ui::frameEnd();
  Serial.printf("views: %s render %lu ms (pack %lu reads %lu ms) page %lu ms\n",
                full ? "full" : "part", (unsigned long)renderMs,
                (unsigned long)pack::statReads,
                (unsigned long)(pack::statMicros / 1000), (unsigned long)pageMs);
}

}  // namespace pokemon_views
