#pragma once

#include <Arduino.h>

// The four Pokemon companion views, laid out on a 680x920 portrait page (the
// 5.76 inch panel's canvas) and scaled down to fit a smaller canvas by
// gbgfx::setLayout().
//
// While a Game Boy game runs in the browser player the page mirrors the
// emulator's work RAM to the reader (see src/games.cpp and src/pokemon_state.*)
// and the reader draws one of these instead of the server status screen. Every
// pixel comes out of the pack at GAMES_PACK_PATH: Game Boy tiles through
// src/gbgfx.*, the game's own font, the game's own text box borders.
//
// The pixel contract is docs/pokemon-aux-display-layout.md. Deviations from it
// are noted at the top of pokemon_views.cpp.
//
// View selection, which games.cpp feeds from the pad state:
//   battle    automatic while the in battle byte is nonzero, whatever the pads
//             say; the previous view comes back by itself when the fight ends
//   terrain   silk up, over whichever page is current
//   home      page 0
//   inventory page 1

namespace pokemon_views {

// Pages the silk right / silk left pads cycle through, wrapping.
static const int PAGE_COUNT = 2;

// Draws one frame. Picks a full refresh when the view changed since the last
// call and a partial one when the same view is redrawing with new data, then
// runs the whole ui::frameBegin / frameNext / frameEnd cycle itself.
void draw(int page, bool terrain);

// Drops the cached pack sections. Call when the pack is opened or closed.
void reset();

}  // namespace pokemon_views
