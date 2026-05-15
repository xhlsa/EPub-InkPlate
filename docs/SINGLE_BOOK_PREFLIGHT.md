# Single-Book Mode — Pre-flight Investigation

Pre-implementation source audit for the `single_book` BUILD_VARIANT.
Covers the five original questions plus five follow-up API questions
resolved before writing any code.

---

## 1. EPub open code path

**Entry:** `books_dir_controller.cpp:186` calls
`book_controller.open_book_file(title, fname, page_id)`

**`BookController::open_book_file`** (`book_controller.cpp:46`):

```
epub.open_file(book_filename)          // open zip, validate, parse OPF metadata
page_locs.stop_document()              // if switching books (sends STOP, blocks on STOPPED)
page_locs.start_new_document(          // tries load() from .locs cache;
    epub.get_item_count(),             //   on miss, enqueues background computation
    page_id.itemref_index)
book_viewer.init()                     // resets viewer state (sets current_page_id = {-1,-1})
page_locs.get_page_id(page_id)         // blocks via retrieve_asap() until starting item ready
→ current_page_id set, return true
```

`PageId` struct (`page_locs.hpp:42`):
```cpp
struct PageId {
  int16_t itemref_index;   // spine index
  int32_t offset;          // byte offset within that item
};
```

**Single-book main.cpp should replicate these calls directly** rather than
going through `BookController`, since `BookController::leave()` and
`input_event()` reference `books_dir_controller` and `app_controller`.

---

## 2. Position persistence

**Format:** Packed 8-byte struct (`nvs_mgr.hpp:18`):
```cpp
struct NVSData {
  int32_t  offset;          // byte offset within item
  int16_t  itemref_index;   // spine index
  uint8_t  was_shown;       // 1 = device went to deep sleep at this position
  uint8_t  filler1;
};
```

**Storage:**
- **On-device:** ESP32 NVS flash, namespace `"EPUB-InkPlate"`.
  Keys `"ID_<n>"` (uint32 book ID) and `"DATA_<n>"` (NVSData as uint64).
  Max 10 books tracked.
- **Linux fallback:** `MAIN_FOLDER/last_book.txt`, 4-line text file
  (filename / itemref_index / offset / was_shown).

**Write:** `nvs_mgr.save_location(book_id, nvs_data)` — `books_dir_controller.cpp:133`
**Read:** `nvs_mgr.get_last(id, nvs_data)` — `books_dir_controller.cpp:47`
         `nvs_mgr.get_location(id, nvs_data)` — by specific book ID

**Book ID:** Jenkins96 hash of the bare filename (not full path).
Defined in `books_dir.cpp:66` as `generate_id(const uint8_t*, uint32_t)`.
→ **Refactor plan:** extract to `src/utils/book_id.hpp` as `inline`.

**`was_shown` semantics:** set to 1 when going to deep sleep, 0 on normal
saves. The full app uses this to know the last-rendered page was fully
displayed before sleep. Single-book mode should match this contract.

---

## 3. page_locs caching

**YES — caches to SD card.** File: `<book-path-without-ext>.locs`

**Binary format** (`page_locs.cpp:1049`, version 3):
```
int8_t   LOCS_FILE_VERSION (= 3)
struct   current_format_params  (~80 bytes: font/screen/orientation settings)
int16_t  page_count
[ per page × page_count ]
  int16_t  itemref_index
  int32_t  offset
  int32_t  size            (negative = no page break at this entry)
```

**Cache load:** `start_new_document()` → `load(epub_filename)` (`page_locs.cpp:776`).
On success the background threads are idle immediately (fast path).

**Cache save:** triggered when background computation completes (`page_locs.cpp:923`).

**Invalidated by:** version mismatch, missing file, or format parameter
change (screen size, font family, font size, orientation). Any of these
causes full recomputation on next open.

**Second-boot with same settings:** instant — `load()` succeeds, no thread work.

---

## 4. Rendering call site

**Signature** (`book_viewer.hpp:62`):
```cpp
void show_page(const PageLocs::PageId & page_id);
```

**Navigation pattern** (`book_controller.cpp:85`):
```cpp
const PageLocs::PageId * next = page_locs.get_next_page_id(current_page_id);
if (next != nullptr) {
    current_page_id = *next;
    book_viewer.show_page(current_page_id);
}
```

**`get_next_page_id` / `get_prev_page_id`** block if the target item's
pages have not been computed yet (same `retrieve_asap()` mechanism).
For a fully-warm cache this is microseconds; for a cold open of a later
chapter it may be seconds on first run.

**Preconditions before calling `show_page`:**
1. `epub.open_file()` has succeeded
2. `page_locs.start_new_document()` has been called
3. `book_viewer.init()` has been called
4. `page_locs.get_page_id(page_id)` has returned non-null (starting item ready)

---

## 5. AppController startup sequence

Minimum init order (from `main.cpp`, InkPlate build):

| # | Call | Constraint |
|---|------|-----------|
| 1 | `nvs_mgr.setup()` | must precede any NVS read/write |
| 2 | `inkplate_platform.setup(true)` | SD card mount, e-ink init |
| 3 | `config.read()` | non-fatal if missing; fonts fall back to index 0 |
| 4 | `pugi::set_memory_management_functions(allocate, free)` | before any XML parse |
| 5 | `page_locs.setup()` | spawns `retriever_thread` + `state_thread` |
| 6 | `fonts.setup()` | **must follow page_locs.setup()**; font metrics drive page breaks |
| 7 | `screen.setup(resolution, orientation)` | |
| 8 | open book / start rendering | |

`fonts.setup()` **requires** `fonts_list.xml` at `/sdcard/fonts_list.xml`
and font files under `/sdcard/fonts/`. No in-binary fallback exists.
`config.read()` failure is safe — `Config::Ident::DEFAULT_FONT` defaults
to 0, selecting the first USER font in the XML.

---

## Follow-up Q&A (five API questions)

### Q1 — page_locs readiness check

```cpp
// page_locs.cpp:397, inside private StateTask class
inline bool retriever_is_iddle() { return retriever_iddle; }
//                       ^^
// [sic] — turgu1's spelling. API-stable; do not "fix".
```

Returns `true` when all pages are computed. **Do not poll this in
single-book main.** The correct pattern is `page_locs.get_page_id()` which
calls `retrieve_asap()` internally and blocks with `portMAX_DELAY` until
the specific item is ready. This is what `open_book_file` already does.

### Q2 — does open_book_file block?

**No — not fully.** `start_new_document()` posts a message to background
threads and returns. However, the final line of `open_book_file` calls
`page_locs.get_page_id(page_id)` which **does block** until the starting
item is paginated.

**After `open_book_file` returns:** the requested starting page is ready.
Background computation of remaining items continues. `get_next_page_id()`
/ `get_prev_page_id()` will block per-item if needed.

**No explicit readiness check is needed anywhere in single-book main.**

### Q3 — nvs_mgr public API

```cpp
// nvs_mgr.hpp:26-32
bool         setup(bool       force_erase = false);
bool save_location(uint32_t   id, const NVSData & nvs_data);
bool      get_last(uint32_t & id,       NVSData & nvs_data);
bool  get_location(uint32_t   id,       NVSData & nvs_data);
bool     id_exists(uint32_t   id);
int8_t     get_pos(uint32_t   id);
bool         erase(uint32_t   id);
```

`setup()` is required first — all other methods return `false` if called
before `initialized` is set.

**Book ID:** computed upstream via Jenkins96 `generate_id()` in
`books_dir.cpp:66`. Input is the **bare filename** (no path). Must use the
same hash to match NVS keys across boots.

### Q4 — scanning /sdcard/books/ for .epub files

Standard POSIX `dirent.h`. Exact pattern from `books_dir.cpp:490`:

```cpp
DIR * dp = opendir(BOOKS_FOLDER);    // BOOKS_FOLDER = "/sdcard/books"
if (dp != nullptr) {
    struct dirent * de;
    while ((de = readdir(dp)) != nullptr) {
        int16_t len = strlen(de->d_name);
        if ((len > 5) && (strcasecmp(&de->d_name[len - 5], ".epub") == 0)) {
            // de->d_name is bare filename; prepend BOOKS_FOLDER "/" for full path
        }
    }
    closedir(dp);
}
```

Single-book mode: stop at the **first match** (no sorting needed).

### Q5 — fonts.setup() minimum invocation

`fonts.setup()` has **no in-binary fallback**. It will return `false` and
abort if either of these is missing:

1. `/sdcard/fonts_list.xml` — valid XML with `<group name="SYSTEM">` and
   `<group name="USER">` containing at least one body font.
2. Font files in `/sdcard/fonts/` matching filenames declared in the XML.

`config.read()` failure is non-fatal: `Config::Ident::DEFAULT_FONT`
integer-defaults to 0, selecting the first USER font. So single-book
mode does **not** need a `config.txt` on the SD card for fonts to work.

**SD layout required** (same as full firmware):
```
/sdcard/
  fonts_list.xml
  fonts/
    *.ttf / *.otf   (as declared in fonts_list.xml)
  books/
    *.epub
```

---

## Implementation plan

### New files
```
src/utils/book_id.hpp              — Jenkins96 generate_id() as inline
src/single_book/main.cpp           — app_main() for single-book mode
src/single_book/wake_button_mgr.hpp/.cpp  — GPIO-36 button state machine
src/single_book/status_overlay.hpp/.cpp   — thin wrapper over ScreenBottom
```

### Modified files
```
src/models/books_dir.cpp    — replace local generate_id with #include "utils/book_id.hpp"
src/CMakeLists.txt          — BUILD_VARIANT option (default "full")
```

### CMake strategy
- `full` (default): GLOB_RECURSE all src/, exclude `src/single_book/`
- `single_book`: GLOB_RECURSE all src/, exclude `src/main.cpp`
  (controllers compile but are unreachable; linker GC strips dead code)

### Button gestures (GPIO 36, active LOW, external pull-up)
| Event | Timing |
|-------|--------|
| `EVT_SHORT` | Press < LONG_MS, no second press within DOUBLE_GAP_MS |
| `EVT_DOUBLE` | Second press within DOUBLE_GAP_MS of first release |
| `EVT_LONG` | Hold ≥ LONG_MS (600 ms), released before VLONG_MS |
| `EVT_VLONG` | Hold ≥ VLONG_MS (2500 ms), emitted while still held |

Timing constants:
```cpp
constexpr uint32_t IDLE_POLL_MS          =   100;
constexpr uint32_t DEEP_SLEEP_TIMEOUT_MS = 3 * 60 * 1000;
constexpr uint16_t DEBOUNCE_MS           =    20;
constexpr uint16_t DOUBLE_GAP_MS         =   280;
constexpr uint16_t LONG_MS               =   600;
constexpr uint16_t VLONG_MS              =  2500;
```

Navigation mapping:
- `EVT_SHORT` → next page
- `EVT_DOUBLE` → previous page
- `EVT_LONG` → status overlay (page X/Y + battery)
- `EVT_VLONG` → persist position, deep sleep
- Idle > 3 min → auto deep sleep
