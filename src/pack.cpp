#include "pack.h"

#include <LittleFS.h>

#include "config.h"

namespace {

uint32_t le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

}  // namespace

namespace pack {

uint32_t statReads = 0;
uint32_t statMicros = 0;

void statReset() { statReads = 0; statMicros = 0; }

// ------------------------------------------------------------ Reader

bool Reader::open(const char *path) {
  close();
  if (!path || !LittleFS.exists(path)) return false;
  file_ = LittleFS.open(path, "r");
  if (!file_) return false;

  uint8_t head[8];
  if (file_.read(head, 8) != 8 || memcmp(head, "EPK1", 4) != 0) {
    file_.close();
    return false;
  }
  uint32_t n = le32(head + 4);
  if (n == 0 || n > (uint32_t)MAX_SECTIONS) {
    file_.close();
    return false;
  }

  uint32_t fileSize = (uint32_t)file_.size();
  sections_ = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint8_t rec[24];
    if (file_.read(rec, 24) != 24) {
      file_.close();
      sections_ = 0;
      return false;
    }
    Entry &e = toc_[sections_];
    memcpy(e.name, rec, 16);
    e.name[16] = 0;
    e.offset = le32(rec + 16);
    e.length = le32(rec + 20);
    // A truncated or lying entry is dropped rather than taking the pack down.
    if (e.offset > fileSize || e.length > fileSize - e.offset) continue;
    sections_++;
  }
  if (!sections_) {
    file_.close();
    return false;
  }
  opened_ = true;
  Serial.printf("pack: %s open, %d sections\n", path, sections_);
  return true;
}

void Reader::close() {
  if (file_) file_.close();
  opened_ = false;
  sections_ = 0;
}

int Reader::find(const char *name) const {
  if (!opened_ || !name) return -1;
  for (int i = 0; i < sections_; i++) {
    if (strcmp(toc_[i].name, name) == 0) return i;
  }
  return -1;
}

size_t Reader::size(const char *name) const { return sizeAt(find(name)); }

size_t Reader::sizeAt(int index) const {
  if (index < 0 || index >= sections_) return 0;
  return (size_t)toc_[index].length;
}

size_t Reader::read(const char *name, size_t offset, void *dst, size_t len) {
  return readAt(find(name), offset, dst, len);
}

size_t Reader::readAt(int index, size_t offset, void *dst, size_t len) {
  if (index < 0 || index >= sections_ || !dst || !len) return 0;
  const Entry &e = toc_[index];
  if (offset >= e.length) return 0;
  size_t room = (size_t)e.length - offset;
  if (len > room) len = room;
  size_t target = (size_t)e.offset + offset;
  uint32_t t0 = micros();
  if (file_.position() != target && !file_.seek(target)) return 0;
  size_t n = file_.read((uint8_t *)dst, len);
  statReads++;
  statMicros += micros() - t0;
  return n;
}

const char *Reader::nameAt(int index) const {
  if (index < 0 || index >= sections_) return "";
  return toc_[index].name;
}

// ------------------------------------------------------------ default pack

namespace {
Reader defaultPack;
}  // namespace

bool open() { return defaultPack.open(GAMES_PACK_PATH); }
void close() { defaultPack.close(); }
bool isOpen() { return defaultPack.isOpen(); }
bool has(const char *name) { return defaultPack.has(name); }
size_t size(const char *name) { return defaultPack.size(name); }
size_t read(const char *name, size_t offset, void *dst, size_t len) {
  return defaultPack.read(name, offset, dst, len);
}
int count() { return defaultPack.count(); }
const char *nameAt(int index) { return defaultPack.nameAt(index); }

}  // namespace pack
