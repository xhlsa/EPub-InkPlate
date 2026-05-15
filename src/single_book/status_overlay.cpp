// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#include "single_book/status_overlay.hpp"

#include "viewers/screen_bottom.hpp"
#include "viewers/page.hpp"
#include "models/page_locs.hpp"

// get_page_nbr() returns -1 if pagination is still in progress (shows
// "PgCalc...%" in ScreenBottom). get_page_count() blocks until the
// background task can report a count, then returns promptly on warm cache.
void StatusOverlay::show(const PageLocs::PageId & page_id)
{
  int16_t page_nbr   = page_locs.get_page_nbr(page_id);
  int16_t page_count = page_locs.get_page_count();
  ScreenBottom::show(page_nbr, page_count);
  page.paint(false);   // partial refresh — preserve book content
}
