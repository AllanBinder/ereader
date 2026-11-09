#include "sd_card.h"

#include <SD.h>
#include <SPI.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "news_sync.h"
#include "pins.h"

namespace {

// Scratch names inside SD_CONVERTED_DIR. The leading dot keeps them out of
// the slug space (a slug never starts with a dot).
#define SD_STAGE_NAME ".stage"
#define SD_BLOB_FILE SD_CONVERTED_DIR "/.blob"

SPIClass *sdSpi = nullptr;
bool isMounted = false;
bool begun = false;

struct Ready {
  String slug;
};
std::vector<Ready> ready;
int skipped = 0;

void feedWdt() { esp_task_wdt_reset(); }

// Card slug to the bare name used for its files in SD_CONVERTED_DIR.
String nameOf(const String &slug) {
  return slug.substring(strlen(CARD_SLUG_PREFIX));
}

String srcPath(const String &slug) {
  return String(SD_CONVERTED_DIR "/") + nameOf(slug) + ".src";
}

// The .src sidecar holds the byte size of the .txt the book was made from,
// as decimal text. A different size means the .txt changed.
bool srcMatches(const String &slug, uint32_t txtBytes) {
  fs::File f = SD.open(srcPath(slug).c_str(), "r");
  if (!f) return false;
  String s = f.readStringUntil('\n');
  f.close();
  s.trim();
  return s.length() && (uint32_t)s.toInt() == txtBytes;
}

void writeSrc(const String &slug, uint32_t txtBytes) {
  fs::File f = SD.open(srcPath(slug).c_str(), "w");
  if (!f) return;
  f.print((unsigned long)txtBytes);
  f.close();
}

// A converted book this boot can use as is: it opens, it was made from a .txt
// of this size, and it is on the active panel's grids.
bool upToDate(const String &slug, uint32_t txtBytes) {
  if (!SD.exists(sd_card::bookPath(slug).c_str())) return false;
  if (!srcMatches(slug, txtBytes)) return false;
  Book b;
  return b.open(slug) && !b.needsReformat();
}

// The page in `to` holding the character stream position `page` of variant
// `variant` started at in `from`, so a book reconverted for another panel
// keeps its reading place. 0 when either book has no anchors.
uint32_t mapAcross(Book &from, Book &to, uint32_t page, uint8_t variant) {
  if (variant >= from.variantCount() || variant >= to.variantCount()) return 0;
  from.setVariant(variant);
  to.setVariant(variant);
  const uint32_t anchor = from.anchorOf(page);
  uint32_t best = 0;
  for (uint32_t p = 0; p < to.pageCount(); p++) {
    if (to.anchorOf(p) > anchor) break;
    best = p;
  }
  return best;
}

// Paginates the .txt at `txtPath` into the card book `slug`. Built under a
// staged name and only swapped in once it opens, so a failure leaves any
// earlier conversion as it was.
bool convert(const String &txtPath, const String &slug, const String &title,
             uint32_t txtBytes, String *err) {
  const String stageSlug = String(CARD_SLUG_PREFIX) + SD_STAGE_NAME;
  const String stagePath = sd_card::bookPath(stageSlug);
  SD.remove(stagePath.c_str());
  uint32_t pages[NEWS_VARIANT_COUNT] = {0};
  if (!news_sync::writeTextBookOn(SD, txtPath.c_str(), SD_BLOB_FILE, stagePath,
                                  title, pages, err)) {
    SD.remove(stagePath.c_str());
    return false;
  }
  feedWdt();
  Book fresh;
  if (!fresh.open(stageSlug)) {
    *err = "did not verify: " + fresh.errorText();
    SD.remove(stagePath.c_str());
    return false;
  }

  // Same text on a new grid (a panel switch): carry the reading place over by
  // stream anchor. A changed .txt starts again at page 1.
  bool keepPos = false;
  uint32_t page = 0;
  uint8_t variant = 0;
  if (srcMatches(slug, txtBytes)) {
    Book old;
    if (old.open(slug)) {
      page = books::loadPosition(slug, &variant);
      if (variant >= old.variantCount()) variant = 0;
      old.setVariant(variant);
      if (page >= old.pageCount()) page = 0;
      page = mapAcross(old, fresh, page, variant);
      keepPos = true;
    }
  }
  fresh.close();

  SD.remove(sd_card::bookPath(slug).c_str());
  SD.remove(sd_card::posPath(slug).c_str());
  if (!SD.rename(stagePath.c_str(), sd_card::bookPath(slug).c_str())) {
    SD.remove(stagePath.c_str());
    *err = "rename failed";
    return false;
  }
  writeSrc(slug, txtBytes);
  if (keepPos) books::savePosition(slug, page, variant);
  return true;
}

struct Found {
  String path;
  String stem;
  uint32_t bytes;
};

}  // namespace

namespace sd_card {

bool fitted() { return PIN_SD_CS >= 0; }

bool begin() {
  if (begun) return isMounted;
  begun = true;
  if (!fitted()) return false;
  sdSpi = new SPIClass(HSPI);  // SPI3; the panel has SPI2 (the global SPI)
  sdSpi->begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, *sdSpi, SD_SPI_HZ) || SD.cardType() == CARD_NONE) {
    SD.end();
    sdSpi->end();
    Serial.printf("card: none\n");
    return false;
  }
  isMounted = true;
  Serial.printf("card: mounted, %lu MB\n",
                (unsigned long)(SD.cardSize() / (1024ULL * 1024ULL)));
  return true;
}

bool mounted() { return isMounted; }

fs::FS &fs() { return SD; }

int prepare(Progress progress) {
  ready.clear();
  skipped = 0;
  if (!isMounted) return 0;

  SD.mkdir(SD_BOOKS_DIR);
  SD.mkdir(SD_CONVERTED_DIR);

  // Collect first: converting writes to the card, which is no time to be
  // walking a directory handle.
  std::vector<Found> found;
  {
    fs::File dir = SD.open(SD_BOOKS_DIR);
    if (!dir || !dir.isDirectory()) return 0;
    fs::File entry = dir.openNextFile();
    while (entry && (int)found.size() < SD_MAX_BOOKS) {
      String name = entry.name();
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      String lower = name;
      lower.toLowerCase();
      // A leading dot is a hidden file, including the "._name.txt" twins
      // macOS writes next to every file it copies onto a FAT card.
      if (!entry.isDirectory() && !name.startsWith(".") && lower.endsWith(".txt")) {
        Found f;
        f.path = String(SD_BOOKS_DIR "/") + name;
        f.stem = name.substring(0, name.length() - 4);
        f.bytes = (uint32_t)entry.size();
        found.push_back(f);
      }
      entry.close();
      entry = dir.openNextFile();
      feedWdt();
    }
    dir.close();
  }

  // Which ones need converting, so progress can say "2 of 3".
  std::vector<int> todo;
  std::vector<String> slugs;
  for (size_t i = 0; i < found.size(); i++) {
    String slug = String(CARD_SLUG_PREFIX) + books::slugify(found[i].stem);
    bool dup = false;
    for (size_t k = 0; k < slugs.size(); k++) dup = dup || slugs[k] == slug;
    slugs.push_back(dup ? String() : slug);
    if (dup || found[i].bytes == 0 || found[i].bytes > SD_MAX_TXT_BYTES) {
      Serial.printf("card: skipped %s (%s)\n", found[i].path.c_str(),
                    dup ? "same name as another book"
                        : (found[i].bytes ? "too big" : "empty"));
      skipped++;
      continue;
    }
    if (upToDate(slug, found[i].bytes)) {
      ready.push_back({slug});
    } else {
      todo.push_back((int)i);
    }
    feedWdt();
  }

  for (size_t t = 0; t < todo.size(); t++) {
    const Found &f = found[todo[t]];
    const String &slug = slugs[todo[t]];
    String title = books::titleFromStem(f.stem);
    if (progress) progress(title, (int)t, (int)todo.size());
    uint32_t t0 = millis();
    String err;
    if (convert(f.path, slug, title, f.bytes, &err)) {
      Serial.printf("card: converted %s in %lu ms\n", f.path.c_str(),
                    (unsigned long)(millis() - t0));
      ready.push_back({slug});
    } else {
      Serial.printf("card: %s not converted: %s\n", f.path.c_str(), err.c_str());
      skipped++;
    }
    feedWdt();
  }
  Serial.printf("card: %d books, %d skipped\n", (int)ready.size(), skipped);
  return (int)ready.size();
}

std::vector<BookEntry> list() {
  std::vector<BookEntry> out;
  if (!isMounted) return out;
  for (size_t i = 0; i < ready.size(); i++) {
    Book b;
    if (!b.open(ready[i].slug)) continue;
    BookEntry e;
    e.slug = ready[i].slug;
    e.title = b.title();
    e.variantCount = b.variantCount();
    e.needsReformat = b.needsReformat();
    e.onCard = true;
    uint8_t variant = 0;
    e.savedPage = books::loadPosition(e.slug, &variant);
    if (variant >= e.variantCount) variant = 0;
    b.setVariant(variant);
    e.savedVariant = variant;
    e.pageCount = b.pageCount();
    if (e.savedPage >= e.pageCount) e.savedPage = 0;
    out.push_back(e);
  }
  return out;
}

int bookCount() { return (int)ready.size(); }

int skippedCount() { return skipped; }

String stateText() {
  if (!isMounted) return String();
  int n = (int)ready.size();
  String s = "card: " + String(n) + (n == 1 ? " book" : " books");
  if (skipped) s += ", " + String(skipped) + " skipped";
  return s;
}

String bookPath(const String &slug) {
  return String(SD_CONVERTED_DIR "/") + nameOf(slug) + ".pgs";
}

String posPath(const String &slug) {
  return String(SD_CONVERTED_DIR "/") + nameOf(slug) + ".pos";
}

}  // namespace sd_card
