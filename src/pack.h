#pragma once

#include <Arduino.h>
#include <FS.h>

// Reader for the EPK1 asset container.
//
// The pack carries the Pokemon graphics and tables the companion screen draws
// with (tiles, font, borders, name tables). It is generated on a host by
// tools/pokered_pack.py from a pokered checkout and uploaded through the games
// page like a ROM, landing at GAMES_PACK_PATH. The repo never ships the assets.
//
// Layout, all little endian:
//
//   0   4   magic "EPK1"
//   4   4   section count
//   8   n*24 entries: char name[16] zero padded, u32 offset, u32 length
//           (offset is from the start of the file)
//   ...     raw section bytes
//
// Only the table of contents is held in RAM. Section bytes are read on demand
// from the open File, so a view can blit a tile without a 40 KB buffer.

namespace pack {

// One open EPK1 file. The free functions below drive a default instance on
// GAMES_PACK_PATH (the Pokemon companion's pack, unchanged call sites).
// Two readers can be open at the same time; each holds its own File and table
// of contents (about 1.2 KB).
class Reader {
 public:
  static const int MAX_SECTIONS = 48;

  // Opens `path` and parses the table of contents. False when the file is
  // absent or not an EPK1 pack; every other call then behaves as if empty.
  bool open(const char *path);
  void close();
  bool isOpen() const { return opened_; }

  // Section index for `name`, -1 when absent. Index based calls skip the
  // name compare on hot paths (icons, map rows).
  int find(const char *name) const;
  bool has(const char *name) const { return find(name) >= 0; }
  size_t size(const char *name) const;  // 0 when absent
  size_t sizeAt(int index) const;
  size_t read(const char *name, size_t offset, void *dst, size_t len);
  size_t readAt(int index, size_t offset, void *dst, size_t len);

  int count() const { return sections_; }
  const char *nameAt(int index) const;

 private:
  struct Entry {
    char name[17];
    uint32_t offset;
    uint32_t length;
  };
  fs::File file_;
  Entry toc_[MAX_SECTIONS];
  int sections_ = 0;
  bool opened_ = false;
};

// Opens GAMES_PACK_PATH and parses the table of contents. False when the file
// is absent or not an EPK1 pack; every other call then behaves as if empty.
bool open();
void close();
bool isOpen();

bool has(const char *name);
size_t size(const char *name);   // 0 when absent

// Read counters since the last statReset(), for profiling a screen draw.
// Every Reader adds to them.
extern uint32_t statReads;
extern uint32_t statMicros;
void statReset();

// Reads `len` bytes from `offset` inside the named section. Returns the number
// of bytes actually read, which is short or 0 on a bad name or a range past
// the end of the section.
size_t read(const char *name, size_t offset, void *dst, size_t len);

// Section count and the name of one section, for the diagnostics line.
int count();
const char *nameAt(int index);

}  // namespace pack
