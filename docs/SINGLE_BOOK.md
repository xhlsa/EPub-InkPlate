# Single-Book Mode

A BUILD_VARIANT that strips the full multi-book controller stack down to
a one-button EPub reader. Intended for a dedicated device where a single
.epub is loaded onto the SD card and the reader just opens it.

---

## Building

```sh
# Single-book firmware
idf.py build -DDEVICE=INKPLATE_5V2 -DAPP_VERSION=2.1.0 \
             -DBUILD_VARIANT=single_book

# Full firmware (default, unchanged)
idf.py build -DDEVICE=INKPLATE_5V2 -DAPP_VERSION=2.1.0
```

`BUILD_VARIANT` defaults to `"full"` if omitted. The two variants share
all models, viewers, and helpers; only the entry point and controllers
differ.

---

## SD card layout (same as full firmware)

```
/sdcard/
  fonts_list.xml          ← required; no in-binary fallback
  fonts/                  ← font files declared in fonts_list.xml
  books/
    your-book.epub        ← first .epub found by readdir() is opened
```

`config.txt` is optional. If missing, screen orientation defaults to
TOP, resolution to ONE_BIT, and the first USER font in `fonts_list.xml`
is used (font index 0).

---

## Button gestures (GPIO 36, active LOW, external pull-up)

| Gesture | Action |
|---------|--------|
| Short press | Next page |
| Double press | Previous page |
| Long press (≥ 600 ms) | Status overlay — page X/Y + battery |
| Very long press (≥ 2500 ms) | Save position → deep sleep |
| Idle 3 min | Auto deep sleep |

Wake from sleep: press GPIO 36 (same button). Position is restored from NVS.

---

## NVS position compatibility

Position records are keyed by `generate_id()` (Jenkins96 hash of the
bare filename). The single-book firmware uses the same hash as
`BooksDirController`, so switching between full and single-book firmware
on the same device preserves the last-read position in both directions.

---

## Design decisions

### Why not reuse `BookController`?

`BookController::leave()` calls `books_dir_controller.save_last_book()`
and `input_event()` calls `app_controller.set_controller()`. Including
`book_controller.cpp` would pull in the full controller graph at link
time. Instead, `single_book/main.cpp` replicates the four lines of
`open_book_file()` that matter (epub.open_file → start_new_document →
book_viewer.init → get_page_id) and calls page_locs / book_viewer
directly in the event loop.

### Why not use the existing EventMgr / touch_event_mgr?

The full `EventMgr` is built around the InkPlate 5V2's capacitive touch
controller (I²C, interrupt-driven, complex state machine for swipe/tap
disambiguation). The single-book device has one physical button on GPIO
36 and no touch screen. Pulling in `event_mgr.cpp` and
`touch_event_mgr.cpp` would initialise hardware that isn't present and
waste ~10 KB of task stack. `WakeButtonMgr` is a 120-line self-contained
polling task that does exactly what is needed and nothing else.

### Why polled (5 ms) instead of interrupt-driven?

GPIO 36 on ESP32 is RTC GPIO / ADC input. It supports edge interrupts,
but a 5 ms polling task is simpler (no ISR, no ISR-safe queue), uses
negligible CPU (task is suspended between polls via vTaskDelay), and is
immune to the class of bugs where an ISR fires during early init before
the queue is ready. At 5 ms granularity the worst-case timing error on a
280 ms double-tap window is 1.8 % — imperceptible.

### Why the DRAIN state (ignoreUntilRelease)?

Without DRAIN, releasing a very-long press triggers a DEBOUNCE_UP →
WAIT_SECOND transition, which then emits EVT_SHORT 280 ms later. The
user presses to sleep and the device wakes up, navigates forward, and
goes back to sleep — three unintended actions from one gesture. DRAIN
absorbs everything after a LONG, VLONG, or DOUBLE event until the button
is fully released.

### Why preserve the `retriever_is_iddle()` typo?

It is the name of a method on a class defined in `page_locs.cpp`. The
string `retriever_is_iddle` appears in at least three call sites in the
existing codebase. Renaming it would be a valid upstream cleanup but is
out of scope for this fork. Our code does not call it (we use
`get_page_id()` which blocks internally), but any future reader of
`page_locs.hpp` should know the spelling is intentional. There is a
`[sic]` comment at the declaration site.

### Why single-book instead of EXTENDED_CASE?

`EXTENDED_CASE` is a compile flag for the InkPlate 6/10 physical keypad
variant. It remaps PREV/NEXT events and requires the full EventMgr +
PressKeys driver chain. Single-book mode targets the InkPlate 5V2, which
has neither a keypad nor a touch screen accessible in the stripped
build — only the single WakeUp button on GPIO 36.

### Why Jenkins96 in a shared header rather than a .cpp?

`generate_id()` is a pure function (no state, no includes beyond
`<cstdint>`). Putting it in a header as `inline` avoids a new `.cpp`
compilation unit and an ODR exposure — the function is small enough that
the compiler will inline it at both call sites (books_dir.cpp and
single_book/main.cpp). The old `#if 0` / `#else` block in
`books_dir.cpp` that dead-coded a CRC32 alternative was deleted in the
same change.

---

## Files

```
src/single_book/
  main.cpp               entry point (replaces src/main.cpp)
  wake_button_mgr.hpp    WakeButtonMgr class
  wake_button_mgr.cpp    polling state machine + FreeRTOS task
  status_overlay.hpp     StatusOverlay::show() declaration
  status_overlay.cpp     ScreenBottom + page.paint(false) wrapper

src/utils/
  book_id.hpp            inline Jenkins96 generate_id()

src/models/
  books_dir.cpp          (modified) uses book_id.hpp instead of local copy

src/CMakeLists.txt       (modified) BUILD_VARIANT option
```
