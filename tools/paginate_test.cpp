// Host harness for the on-device paginator in src/text_paginate.cpp.
//
// Reads UTF-8 text on stdin, paginates it on the given grid, and prints the
// pages in the same form tools/check_paginator.py prints them from the Python
// pipeline, so the two can be diffed directly:
//
//     c++ -std=c++11 -O1 -I../src tools/paginate_test.cpp \
//         src/text_paginate.cpp -o /tmp/paginate_test
//     python3 tools/check_paginator.py sample.txt
//
//     paginate_test COLS ROWS                    paginate stdin on COLS x ROWS
//     paginate_test COLS ROWS SRC_COLS SRC_ROWS  paginate on SRC first, then
//                                                re-paginate those pages on
//                                                COLS x ROWS the way
//                                                books::reformat does
//
// Each page header carries the page's anchor, so the check can compare those
// too. This file is not part of the firmware: it lives in tools/, so
// PlatformIO never compiles it into the esp32s3 build.

#include <stdio.h>
#include <stdlib.h>

#include <string>
#include <vector>

#include "text_paginate.h"

int main(int argc, char **argv) {
  int cols = (argc > 1) ? atoi(argv[1]) : 54;
  int rows = (argc > 2) ? atoi(argv[2]) : 37;
  bool reflow = argc > 4;
  int srcCols = reflow ? atoi(argv[3]) : cols;
  int srcRows = reflow ? atoi(argv[4]) : rows;

  int pageNo = 0;
  textpage::Pipeline *outPtr = nullptr;
  textpage::Pipeline out(cols, rows,
                         [&](const std::vector<std::string> &lines) {
    printf("=== page %d (%d lines) anchor %u\n", pageNo++, (int)lines.size(),
           (unsigned)outPtr->pageAnchor());
    for (size_t i = 0; i < lines.size(); i++) {
      std::string utf8 = textpage::encodeUtf8(lines[i]);
      printf("%s\n", utf8.c_str());
    }
  });
  outPtr = &out;

  // Reflow mode: the source grid's pages, as UTF-8 page blobs exactly as they
  // would sit in a .pgs file.
  std::vector<std::string> srcPages;
  textpage::Pipeline src(srcCols, srcRows,
                         [&](const std::vector<std::string> &lines) {
    std::string joined;
    for (size_t i = 0; i < lines.size(); i++) {
      if (i) joined.push_back('\n');
      joined += lines[i];
    }
    srcPages.push_back(textpage::encodeUtf8(joined));
  });
  textpage::Pipeline &first = reflow ? src : out;

  unsigned char buf[997];  // deliberately not a power of two: exercises the
                           // decoder's carry across chunk boundaries
  size_t got;
  while ((got = fread(buf, 1, sizeof(buf), stdin)) > 0) {
    first.feed(buf, got);
  }
  first.finish();

  if (reflow) {
    textpage::Rejoiner rejoin(srcCols, [&](const std::string &para) {
      out.paragraph(para);
    });
    for (size_t i = 0; i < srcPages.size(); i++) {
      const std::string &blob = srcPages[i];
      rejoin.page(textpage::decodeLatin1((const uint8_t *)blob.data(),
                                         blob.size()));
    }
    rejoin.finish();
    out.endRun();
    fprintf(stderr, "source pages: %u  reflowed pages: %d\n",
            (unsigned)srcPages.size(), pageNo);
  }

  fprintf(stderr, "pages: %u  truncated: %d\n",
          (unsigned)first.pageCount(), first.truncated() ? 1 : 0);
  return 0;
}
