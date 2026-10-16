#pragma once

#include <Arduino.h>

#include <vector>

// Daily numbers: the metrics hub's cross project figures, drawn as a grid of
// cards in the same family as the menu.
//
// The data is /numbers.json (NUMBERS_FILE in config.h), stored verbatim by the
// feed sync from GET /metrics/ereader.json on the Worker. Every string in it is
// already formatted for the glass (ASCII, labels at most 14 characters, values
// at most 9, deltas signed), so this module does no number formatting: it
// parses, holds, and draws.
//
//   {"day":"2026-09-10","label":"Thu 10 Sep","generated":"08:07",
//    "windows":[{"name":"Yesterday","range":"Thu 10 Sep"},
//               {"name":"7 days","range":"4 Sep to 10 Sep"},
//               {"name":"30 days","range":"12 Aug to 10 Sep"},
//               {"name":"Total","range":"since 9 Sep"}],
//    "projects":[{"id":"muon","name":"Muon Sortes",
//      "headline":["Visits up 144%","Visits up 18%","Quiet day.",
//                  "Everything since Wed 9 Sep."],
//      "metrics":[{"l":"Visits","v":["22","140","500","900"],
//                  "a":["7d avg 9","prev 117","prev 400","avg 15/day"],
//                  "d":["+144%","+20%","+25%",null]}]}],
//    "notes":["ig skipped","reddit failed"]}
//
// Every metric carries one value, one foot line and one delta per time window
// (parallel arrays, in the order of "windows"); the view shows one window at
// a time and switches with the "right" pad. One screen per project, then the
// ads screen when the feed carries one, then one more listing the notes when
// there are any.
//
// The optional top level "ads" key is the paid campaign block: one screen of
// its own, a column per channel (Apple Search Ads, Google Ads) and a row per
// metric, so the same metric sits side by side across channels. A feed
// without it behaves exactly as before.
//
//   "ads":{"name":"Tinh ads",
//          "headline":["$41.20 for 12 installs","..","..",".."],
//          "channels":[{"id":"asa","name":"Apple Search Ads",
//                       "metrics":[{"l":"Spend","v":["$41.20",..],..}]},
//                      {"id":"gads","name":"Google Ads","metrics":[..]}]}
// Screen layout (portrait, 680x920 on panel 0 and 480x800 on the 7.5 inch
// panel; constants and the per-panel arithmetic in numbers_view.cpp):
//
//   black header band: project name (helvB24, white), "7 days  4 Sep to 10 Sep"
//   headline line (profont22), one per window
//   2 x 5 grid of cards, one metric each: label (profont22), accent hairline,
//     value (fub42, or helvB24 when it carries a letter or does not fit,
//     with a "$" prefix or a "k"/"M" suffix in helvB24 either side of it),
//     the foot line bottom left and the delta bottom right (7x14)
//   status strip: "daily numbers  2/5  08:07" and the button hint (a short
//     hint, then no time, when the strip is too narrow for both)
//
// The ads screen adds a channel name line (profont22) between the headline
// and the grid, which starts a little lower and has slightly shorter tiles.

namespace numbers_view {

// Windows the feed may carry. Anything past this is ignored.
static const int MAX_WINDOWS = 4;

struct Window {
  String name;   // "7 days"
  String range;  // "4 Sep to 10 Sep"
};

struct Metric {
  String label;
  String value[MAX_WINDOWS];  // "-" when the feed had nothing
  String foot[MAX_WINDOWS];   // empty when the feed sent null
  String delta[MAX_WINDOWS];  // empty when there is no comparison
  // Daily values for the chart window, on the shared axis: hist[i] is day
  // histOffset + i of Data::historyDays, NAN for a day with no row.
  std::vector<float> hist;
  int histOffset = 0;
};

struct Tick {
  int index;     // day index on the history axis
  String label;  // "9 Sep"
};

struct Project {
  String id;
  String name;
  String headline[MAX_WINDOWS];
  std::vector<Metric> metrics;
};

// One paid channel of the ads screen: a column of the grid.
struct Channel {
  String id;    // "asa"
  String name;  // "Apple Search Ads"
  std::vector<Metric> metrics;  // Spend, Installs, Cost/install, Taps, Impressions
};

// The ads block. `present` is false for a feed without an "ads" key, and the
// view then behaves exactly as it did before the block existed.
struct Ads {
  String name;  // "Tinh ads", the header band title
  String headline[MAX_WINDOWS];
  std::vector<Channel> channels;  // 1 or 2, column 0 first
  bool present = false;
};

struct Data {
  String day;        // "2026-09-10", the reporting day
  String label;      // "Thu 10 Sep"
  String generated;  // "08:07", ICT clock of the render
  std::vector<Window> windows;  // at least one after load()
  int historyDays = 0;          // length of the chart axis, 0 = no history
  std::vector<Tick> ticks;      // x axis labels for the chart window
  std::vector<Project> projects;
  Ads ads;
  std::vector<String> notes;

  // Screens to page through: one per project, then the ads screen when the
  // feed carries one, plus the notes screen when there are notes. Never less
  // than one, so an empty feed still shows a "nothing in the window" screen
  // instead of nothing.
  int screens() const;

  // Index of the ads screen, or -1 when the feed has no ads block.
  int adsScreen() const;
};

// Metrics reachable on screen `screen`: a project's own metrics, the ads
// screen's channels flattened (channel 0 then channel 1), 0 for the notes or
// empty screen. This is what the chart window pages through, so the
// navigation in main.cpp asks here instead of indexing projects directly.
int metricsOnScreen(const Data &data, int screen);

// True when NUMBERS_FILE exists, so the caller can tell "never synced" from
// "failed to parse" before spending a frame.
bool available();

// Parses NUMBERS_FILE into `out`. Returns false and sets `err` on any failure
// (missing file, bad JSON, wrong shape); `out` is then cleared.
bool load(Data &out, String *err);

// Draws screen `index` of `data` (clamped to 0..screens()-1) in time window
// `window` (clamped to the windows the feed carries). Same refresh policy as
// the menu: partial unless `forceFull`, with the usual ration and the image
// page carry over handled inside ui.
void render(const Data &data, int index, int window, bool forceFull = false);

// The chart window: one full screen line chart of metric `metric` of screen
// `index` over the history axis. Value labels at the left, date ticks along
// the bottom, a dot per day and a line between consecutive days that both
// have a value. On the ads screen the metrics are flattened channel 0 then
// channel 1 and the chart is titled "<channel>: <label>". `index` on the
// notes screen draws the notes screen instead.
void renderChart(const Data &data, int index, int metric, bool forceFull = false);

// The status screen shown when there is nothing to draw: no file yet, or a
// file that did not parse (`err` is printed when non empty).
void renderMissing(const String &err, bool forceFull = false);

}  // namespace numbers_view
