# EPub-InkPlate — Developer Notes

Target: InkPlate 5 Gen 2 (INKPLATE_5V2), single_book BUILD_VARIANT.

## Build & Flash

```bash
# Source ESP-IDF (Python 3.13 venv, not system 3.14)
source ~/.espressif/python_env/idf5.5_py3.13_env/bin/activate
source ~/esp/esp-idf/export.sh

cd ~/EPub-InkPlate

# Build
idf.py -C . -B build_single -D BUILD_VARIANT=single_book build

# Flash (InkPlate is CH340 on ttyUSB0, NOT ttyACM0 which is the Steam Deck controller)
idf.py -C . -B build_single -p /dev/ttyUSB0 flash

# Monitor
python3 ~/monitor.py /dev/ttyUSB0 115200
# or: idf.py -C . -B build_single -p /dev/ttyUSB0 monitor

# Erase NVS only (use after changing orientation or epub filename)
esptool.py --chip esp32 --port /dev/ttyUSB0 erase_region 0x9000 0x4000

# Full chip erase + reflash (use when NVS is corrupt or iterator crashes)
esptool.py --chip esp32 --port /dev/ttyUSB0 erase_flash
idf.py -C . -B build_single -p /dev/ttyUSB0 flash
```

## USB / Serial

- InkPlate 5 Gen 2: CH340 chip → `/dev/ttyUSB0` (USB ID `1a86:7523`)
- `/dev/ttyACM0` is the Steam Deck controller (ID `28de:1205`) — never flash to it
- If ttyUSB0 is missing: power-cycle the InkPlate; use a data-capable USB cable (not charge-only); try through a powered hub
- `chmod 777 /dev/ttyUSB0` if permission denied (SteamOS doesn't add deck to dialout)

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
- `show_page_full()` calls `screen.force_full_update()` before every render to prevent ghost artifacts from partial updates

### Sleep & Wake
- Deep sleep via `inkplate_platform.deep_sleep(GPIO_NUM_36, 0)` — wake pin is GPIO 36, active LOW
- Wake detection: `esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0`
- **No sleep overlay** — e-ink retains the last page without power; the page IS the sleep indicator
- On wake, skip the "Loading..." splash (panel already shows the page, silent reload feels seamless)
- Auto-sleep after 30 minutes of inactivity

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

## Backtrace Decoding

```bash
# Decode a crash backtrace (replace 0x... with actual addresses from serial)
~/.espressif/tools/xtensa-esp32-elf/*/xtensa-esp32-elf/bin/xtensa-esp32-elf-addr2line \
  -e build_single/EPub-InkPlate.elf -f -i 0x400d1234 0x400d5678
```

## Branch

All single_book work lives on `inkplate-5v2-port`. The `master` branch is the upstream multi-book firmware — do not merge single_book changes back to master without careful review.
