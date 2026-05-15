// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once

#include "global.hpp"
#include "models/page_locs.hpp"

// Draws a status bar (page X / Y + battery icon) over the current screen
// content using a partial refresh. Wraps ScreenBottom::show() + page.paint().
namespace StatusOverlay {
  void show(const PageLocs::PageId & page_id);
}
