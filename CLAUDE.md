# EPub-InkPlate — Developer Notes

Target: InkPlate 5 Gen 2 (INKPLATE_5V2), single_book BUILD_VARIANT.

## Build & Flash

```bash
# Source ESP-IDF (Python 3.13 venv, not system 3.14)
source ~/.espressif/python_env/idf5.5_py3.13_env/bin/activate
source ~/esp/v5.5.2/export.sh

cd ~/EPub-InkPlate

# Build (DEVICE and APP_VERSION are required — CMakeLists.txt errors without DEVICE)
idf.py -C . -B build_single -D BUILD_VARIANT=single_book -D DEVICE=INKPLATE_5V2 -D APP_VERSION=2.1.2 build

# Flash (InkPlate is CH340 on ttyUSB0, NOT ttyACM0 which is the Steam Deck controller)
idf.py -C . -B build_single -D BUILD_VARIANT=single_book -D DEVICE=INKPLATE_5V2 -D APP_VERSION=2.1.2 -p /dev/ttyUSB0 flash

# Monitor
python3 ~/monitor.py /dev/ttyUSB0 115200
# or: idf.py -C . -B build_single -p /dev/ttyUSB0 monitor

# Erase NVS only (use after changing orientation or epub filename)
esptool.py --chip esp32 --port /dev/ttyUSB0 erase_region 0x9000 0x4000

# Full chip erase + reflash (use when NVS is corrupt or iterator crashes)
esptool.py --chip esp32 --port /dev/ttyUSB0 erase_flash
idf.py -C . -B build_single -D BUILD_VARIANT=single_book -D DEVICE=INKPLATE_5V2 -D APP_VERSION=2.1.2 -p /dev/ttyUSB0 flash
```

## USB / Serial

- InkPlate 5 Gen 2: CH340 chip → `/dev/ttyUSB0` (USB ID `1a86:7523`)
- `/dev/ttyACM0` is the Steam Deck controller (ID `28de:1205`) — never flash to it
- If ttyUSB0 is missing: power-cycle the InkPlate; use a data-capable USB cable (not charge-only); try through a powered hub
- `chmod 777 /dev/ttyUSB0` if permission denied (SteamOS doesn't add deck to dialout)

## Submodule

`components/ESP-IDF-InkPlate` points to `xhlsa/ESP-IDF-InkPlate` (fork of
`turgu1/ESP-IDF-InkPlate`) on the `inkplate-5v2-port` branch. The fork exists
because the upstream cannot receive 5V2-specific fixes.

Fresh clone:
```bash
git clone https://github.com/xhlsa/EPub-InkPlate
cd EPub-InkPlate
git checkout inkplate-5v2-port
git submodule update --init --recursive
```

## SD Card Layout

```
/sdcard/
  fonts_list.xml
  fonts/          ← font files listed in fonts_list.xml
  books/          ← one or more .epub files
  config.txt      ← optional, ignored by single_book for resolution/heap/title
```

Config.txt values like `show_heap=1`, `show_title=1`, `resolution=1` are **ignored** by single_book — compile-time guards override them. Orientation IS read from config (default RIGHT=1, which is portrait); single_book hardcodes LEFT orientation in screen.setup().

## single_book Design Rules

### Orientation
- `Screen::Orientation::LEFT` = 720×1280 portrait, USB port at bottom (180° flip of RIGHT)
- `Screen::Orientation::RIGHT` = 720×1280 portrait, USB port at top
- `Screen::Orientation::TOP` / `BOTTOM` = 1280×720 landscape — **never use**, breaks layout
- LEFT and RIGHT share the same page dimensions so NVS offsets remain valid between them
- **Changing orientation between boots invalidates all saved NVS page offsets** — erase NVS after any orientation change

### Pixel Resolution / Partial Refresh
- Always use `Screen::PixelResolution::ONE_BIT` — THREE_BITS disables partial refresh (10× slower page turns)
- `screen.update()` manages a `partial_count` counter (`PARTIAL_COUNT_ALLOWED = 10`): first render after `screen.setup()` is always a full refresh (`partial_count` initialised to 0), then up to 10 partial refreshes, then a full refresh to clear ghost buildup, and so on
- **Do not call `screen.force_full_update()` before page turns** — it resets the counter to 0, forcing a full refresh (8 clean passes + 5 frame scans, ~2× slower) on every page turn
- `force_full_update()` is appropriate only for the status overlay and message viewer, which paint over live content and need a clean slate

### Progressive Page-Turn Refresh (`page_turn_mode = 2`, the default)

Body page turns go through `book_viewer.cpp:build_page_at()` → `page.paint(skip_update=true)` → dispatch on `config.get(PAGE_TURN_MODE)`:

| mode | behaviour |
|------|-----------|
| 0 | normal — `screen.update(false)`, partial_count managed |
| 1 | force full — `screen.update(true)` |
| 2 | progressive — `screen.full_refresh_progressive()` (**default**) |

`full_refresh_progressive()` (`components/inkplate_screen/src/screen.cpp`):
1. `memcpy` the rendered framebuffer into `capture_buffer_` (PSRAM, allocated in `setup()`)
2. `memset(fb, 0xFF)` → `e_ink.partial_update()` — **black flash** (0xFF = all black in 1-bit)
3. `memset(fb, 0x00)` → `e_ink.partial_update()` — **white flash** (0x00 = all white)
4. `memcpy` capture_buffer_ back → `e_ink.partial_update()` — full page restore

Total: 3 partial waveform cycles per page turn. `partial_count` is set to 0 after each call so the next `screen.update()` call (status overlay, msg_viewer) forces a clean full refresh.

**Bit polarity**: in the 1-bit framebuffer, bit SET = BLACK, bit CLEAR = WHITE. So `0xFF = all black`, `0x00 = all white`. Do not confuse with 3-bit mode.

**Cold-boot behaviour**: on the very first page turn after power-on, `is_partial_allowed()` in the eink driver is false. `e_ink.partial_update()` falls back to the full 8-pass clean sequence for the black flash, producing multiple visible flashes. All subsequent page turns are clean 3-cycle sequences. This is accepted behaviour — no primer is installed.

**Deep-sleep wake**: unknown whether `is_partial_allowed()` persists across deep sleep (ESP32 regular RAM is lost on deep sleep, so likely not). If wake-from-sleep shows the same cold-boot multi-flash, it needs a separate fix — do not conflate with the boot primer decision.

**`capture_buffer_`**: allocated via `heap_caps_malloc(MALLOC_CAP_SPIRAM)` with `malloc()` fallback. Same size as the 1-bit framebuffer (115 200 bytes for 5V2). If `capture_buffer_` is null (allocation failed), `full_refresh_progressive()` falls back to `e_ink.update()`.

**Config keys** (both readable from `/sdcard/config.txt`):
```
page_turn_mode=2        # 0/1/2 — default 2
progressive_stripes=4   # ignored in current implementation, reserved
```

### Sleep & Wake
- Deep sleep via `inkplate_platform.deep_sleep(GPIO_NUM_36, 0)` — wake pin is GPIO 36, active LOW
- Wake detection: `esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0`
- **No sleep overlay** — e-ink retains the last page without power; the page IS the sleep indicator
- Auto-sleep after 30 minutes of inactivity

#### Wake-time render skip (Phase 1)

On `ESP_SLEEP_WAKEUP_EXT0` the initial `show_page()` call is skipped entirely.
The e-ink panel keeps the previous page from retention; the user presses the
button and the normal `full_refresh_progressive()` sequence runs as the first
waveform cycle. On all other reset reasons (cold boot, brownout, reset button)
`show_page()` runs immediately as before.

**`rendered` flag:** a `bool rendered = false` is set to `true` after the first
successful `show_page()`. `EVT_LONG` (status overlay) checks this flag: if still
false on a wake-from-sleep boot, it calls `show_page()` first. Without this,
`partial_allowed` is `false` (reset by deep-sleep reinitialisation), so the
partial call inside the overlay falls back to `e_ink::update()` with an empty
framebuffer — producing a blank white panel instead of the overlay drawn over
the retained page.

**Reason `partial_allowed` is false after wake:** it is a member of the `EInk`
singleton (normal RAM), which is re-initialised to `false` on every boot because
deep sleep loses all non-RTC RAM. `allow_partial()` is only set at the end of
`e_ink.update()` (full waveform cycle). On a wake boot where we skip
`show_page()`, no full cycle runs until the first button press.

**Do not** add a separate "cold-boot primer" full refresh to reset
`partial_allowed` — that's exactly the redundant flash this phase removes.
Let the first user-initiated page turn own the cold/wake full-cycle cost.

### Config Guards (compile-time suppression)
Single_book suppresses UI elements that don't belong in a minimal reader. Do not add runtime config reads for these:

```cpp
// book_viewer.cpp — suppress title bar
int8_t show_title = 0;
#if !SINGLE_BOOK_BUILD
  config.get(Config::Ident::SHOW_TITLE, &show_title);
#endif

// screen_bottom.cpp — suppress heap stats and PgCalc% progress text
#if EPUB_INKPLATE_BUILD && !SINGLE_BOOK_BUILD
  // heap display + BatteryViewer::show()
#endif

#if !SINGLE_BOOK_BUILD
else if (page_count != -1) {
  ostr << "PgCalc... " << page_count << "%";
  // ...
}
#endif
```

## NVS Position Persistence

### How it works
- Book ID = Jenkins96 hash of the **bare epub filename** (not full path)
- `nvs_mgr.save_location(book_id, data)` stores `{itemref_index, offset, was_shown}`
- `nvs_mgr.get_location(book_id, nvs_data)` restores on boot
- NVS partition: 0x9000–0xD000 (16KB), sufficient for 10 books

### Critical bug — books_dir iterator crash (FIXED)
`NVSMgr::save()` and `NVSMgr::remove()` both called `books_dir.set_track_order()`.
In single_book, `books_dir.sorted_index` is empty (uninitialized). The call path was:

```
save() → books_dir.set_track_order(id, pos)
       → sorted_index empty → !found
       → nvs_mgr.erase(id)          ← deletes the entry just written
       → remove() → nvs_erase_key() ← AND invalidates the reverse_iterator
       → LoadProhibited crash at EXCVADDR 0x00000004
```

**Fix**: guard both call sites with `#if !SINGLE_BOOK_BUILD` in `src/models/nvs_mgr.cpp`:

```cpp
// In save(), after track_list[index] = id; track_count++;
#if !SINGLE_BOOK_BUILD
  int8_t pos = 0;
  for (TrackList::reverse_iterator rit = track_list.rbegin(); ...) {
    books_dir.set_track_order(rit->second, pos);
  }
#endif

// In remove(), inside the nvs_get_u32 success block
#if !SINGLE_BOOK_BUILD
  books_dir.set_track_order(the_id, -1);
#endif
```

### NVS iterator leak (pre-existing, benign)
In `NVSMgr::setup()`, if `res != ESP_OK` on the first `nvs_entry_find()` call, `nvs_release_iterator(it)` is still called with an invalid iterator. Harmless in practice (IDF handles null-ish iterators gracefully) but worth noting.

## Power / Battery

### CPU frequency
`sdkconfig.defaults` sets `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=80`. The e-ink loop
is almost entirely blocked on input — 80 MHz is sufficient for I2S-DMA rendering
and cuts active draw from ~68 mA (240 MHz) to ~25 mA. Do not bump it back to
240 MHz; if a future feature genuinely needs more headroom, use `esp_pm_configure`
with DFS rather than raising the static ceiling.

### Deep-sleep GPIO isolation (5V2)
`inkplate_platform.cpp:deep_sleep()` isolates GPIO 0, 2, 32, 33 (the I2S e-ink
bus outputs) and GPIO 12 (MISO) via `rtc_gpio_isolate()` before
`esp_deep_sleep_start()`. The guard is `#if INKPLATE_5V2` — do not remove it.
If new output GPIOs are added to the 5V2 eink bus, add them here too or they
will leak current through the panel circuitry in deep sleep.

### No forced debug logging
`eink_5v2.cpp:setup()` previously called `esp_log_level_set(TAG, ESP_LOG_DEBUG)`
unconditionally, overriding the build-level log config and firing UART writes on
every page turn. Those lines have been removed. If you need debug output from the
eink driver, set the level temporarily in your own code or via `idf.py monitor`
log filters — do not re-add unconditional overrides to the driver.

## EInk Driver — 5V2-Specific Bugs (in submodule)

### Partial-refresh split-screen — `uint16_t pos` overflow (FIXED)
**`components/ESP-IDF-InkPlate/src/drivers/eink_5v2.cpp:351`**

`BITMAP_SIZE_1BIT = (1280 × 720) / 8 = 115 200`, which exceeds `uint16_t` max (65 535).
The original declaration:
```cpp
uint16_t pos = BITMAP_SIZE_1BIT - 1;  // 115199 → truncated to 49663
```
caused the diff fill loop in `partial_update()` to start in the middle of the
framebuffer. Bytes 65 536–115 199 (rows 410–719, the top ~43% of the panel) were
never read; their waveform data in `p_buffer` was computed against byte 0. This
produced correct partial updates on the bottom ~57% of the display and corrupted
updates on the top ~43% — visible as a horizontal split.

The 6V2 driver has identical code but `BITMAP_SIZE_1BIT = 60 000`, which fits in
`uint16_t`, so the bug never manifested there.

**Fix** (committed to `xhlsa/ESP-IDF-InkPlate` on `inkplate-5v2-port`):
```cpp
size_t pos = BITMAP_SIZE_1BIT - 1;
```

When porting driver code from a smaller InkPlate variant to the 5V2, audit every
`uint16_t` used as a framebuffer index or size — the 5V2's 115 200-byte bitmap
exceeds `uint16_t` range, while all smaller panels (6, 6V2, 6PLUS, 10) stay
within it.

## Backtrace Decoding

```bash
# Decode a crash backtrace (replace 0x... with actual addresses from serial)
~/.espressif/tools/xtensa-esp32-elf/*/xtensa-esp32-elf/bin/xtensa-esp32-elf-addr2line \
  -e build_single/EPub-InkPlate.elf -f -i 0x400d1234 0x400d5678
```

## Branch

All single_book work lives on `inkplate-5v2-port`. The `master` branch is the upstream multi-book firmware — do not merge single_book changes back to master without careful review.
