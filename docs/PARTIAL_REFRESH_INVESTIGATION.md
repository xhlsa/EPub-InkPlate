# Partial-Refresh Split-Screen Bug — InkPlate 5V2 Investigation

## 1. Call Path from `book_viewer.show_page()` to the EInk Driver

### Step 1 — `BookViewer::show_page()`
**`src/viewers/book_viewer.cpp:247`**

Dispatches on whether the page is the cover (itemref_index == 0, offset == 0) or a body page:

- Cover path → `page.show_cover(img)` → `page.paint(false, false, true)` at `page.cpp:1206`
- Body path → `BookViewer::build_page_at(page_id)` at `book_viewer.cpp:45`

### Step 2 — `BookViewer::build_page_at()`
**`src/viewers/book_viewer.cpp:45`**

Builds the display list by interpreting the epub XML, draws the page-number bar
via `ScreenBottom::show()`, then calls:

```
page.paint()   // book_viewer.cpp:177
```

### Step 3 — `Page::paint(bool clear_screen, bool no_full, bool do_it)`
**`src/viewers/page.cpp:277`**

Default signature: `paint(true, false, false)`.  
Iterates the display list (glyphs, images, regions), writes pixels into the
framebuffer via `screen.draw_glyph()` / `screen.draw_bitmap()` / `screen.colorize_region()`,
then on line 342:

```cpp
screen.update(no_full);   // page.cpp:342
```

The `no_full` argument is `false` for all normal book-page renders.

### Step 4 — `Screen::update(bool no_full)`
**`components/inkplate_screen/src/screen.hpp:70`**

```cpp
inline void update(bool no_full = false) {
  if (pixel_resolution == PixelResolution::ONE_BIT) {
    if (no_full) {
      e_ink.partial_update(*frame_buffer_1bit);
      partial_count = 0;
    } else {
      if (partial_count <= 0) {
        e_ink.update(*frame_buffer_1bit);       // full refresh
        partial_count = PARTIAL_COUNT_ALLOWED;  // = 10 on INKPLATE_5V2
      } else {
        e_ink.partial_update(*frame_buffer_1bit);
        partial_count--;
      }
    }
  }
}
```

`PARTIAL_COUNT_ALLOWED = 10` for INKPLATE_5V2 (`screen.hpp:36`).  
`force_full_update()` (`screen.hpp:186`) sets `partial_count = 0`, forcing the
next call to take the full-refresh branch.

`partial_count` is initialised to 0 in the `Screen` constructor and in
`set_pixel_resolution()` (`screen.cpp:472`), so the very first `update()` call
after `screen.setup()` is always a full refresh.

### Step 5 — `EInk5V2::partial_update(FrameBuffer1Bit &, bool force)`
**`components/ESP-IDF-InkPlate/src/drivers/eink_5v2.cpp:339`**

If `!is_partial_allowed() && !force`, falls back to `update()` (full refresh).
Otherwise executes the partial waveform described in §3.

### Step 6 — `EInk5V2::update(FrameBuffer1Bit &)`
**`components/ESP-IDF-InkPlate/src/drivers/eink_5v2.cpp:147`**

Eight `clean()` passes (WHITE 1×, BLACK 11×, DISCHARGE 1×, WHITE 11×, DISCHARGE 1×,
BLACK 11×, DISCHARGE 1×, WHITE 11×) followed by three LUTB frame passes and one
LUT2 pass and one blank pass.  Sets `allow_partial()` at line 267 on successful
completion.

### Step 7 — I2S / panel hardware
Both `update()` and `partial_update()` drive the panel with the same sequence:

```
vscan_start()          // eink.cpp:171  — sets SPV/CKV timing
i2s_comms.send_data()  // i2s_comms.cpp:99 — fires DMA transfer of line_buffer
vscan_end()            // eink.cpp:194  — latches row via LE pulse
```

`vscan_start()` is called once per waveform phase; `i2s_comms.send_data()` and
`vscan_end()` are called once per row within each phase.

---

## 2. Dirty-Rectangle Computation

**There is none.**

`EInk5V2::partial_update()` always processes the entire framebuffer.

- **Fill phase** (`eink_5v2.cpp:353–361`): outer loop `i = 0..HEIGHT-1`, inner loop
  `j = 0..LINE_SIZE_1BIT-1` — iterates every byte of both `idata` (new frame) and
  `odata` (previous frame `d_memory_new`).
- **Transmission phase** (`eink_5v2.cpp:383–400`): outer loop `i = 0..HEIGHT-1`,
  inner loop `j = 0..WIDTH/4-1` — sends every row to the panel.

The only "spatial" decision is the binary choice in `Screen::update()` between
`e_ink.update()` (full) and `e_ink.partial_update()` (partial), governed solely
by the `partial_count` counter.

**Loop-bound constants (all in `eink_5v2.hpp:50–57`):**

| Constant | Expression | Value |
|---|---|---|
| `WIDTH` | `1280` (literal) | 1280 pixels |
| `HEIGHT` | `720` (literal) | 720 pixels |
| `LINE_SIZE_1BIT` | `WIDTH >> 3` | 160 bytes/row |
| `BITMAP_SIZE_1BIT` | `(WIDTH * HEIGHT) >> 3` | 115 200 bytes |

---

## 3. 5V2 Partial-Refresh Implementation

### Memory layout

Allocated in `EInk5V2::begin()` (`eink_5v2.cpp:113–114`):

```cpp
d_memory_new = new_frame_buffer_1bit();            // 115 200 bytes
p_buffer     = (uint8_t *)malloc(BITMAP_SIZE_1BIT * 2);  // 230 400 bytes
```

`d_memory_new` stores the last frame sent to the panel (used as "old" in the diff).
`p_buffer` stores the LUT-transformed differential data (2 output bytes per input byte,
one per nibble).

### I2S line-buffer sizing

The `EInk` base constructor (`eink.hpp:65–66`) passes `screen_width` from the subclass:

```cpp
EInk(IOExpander &io_expander, const int screen_width)
    : ..., i2s_comms(I2SComms((screen_width / 4) + 16)), ...
```

For the 5V2, `screen_width = WIDTH = 1280`, so the I2S line buffer is:

```
(1280 / 4) + 16 = 336 bytes
```

Allocated via `heap_caps_malloc(336, MALLOC_CAP_DMA)` (`i2s_comms.hpp:53`).  
The DMA descriptor fields `size` and `length` are both set to `line_buffer_size = 336`
at each `init_lldesc()` call (`i2s_comms.cpp:232–233`).

### Fill phase (`eink_5v2.cpp:350–361`)

```cpp
uint32_t n   = BITMAP_SIZE_1BIT * 2 - 1;   // = 230 399
uint16_t pos = BITMAP_SIZE_1BIT - 1;        // ← see §4

for (int i = 0; i < HEIGHT; i++) {
  for (int j = 0; j < LINE_SIZE_1BIT; j++) {
    uint8_t diffw = odata[pos] & ~idata[pos];
    uint8_t diffb = ~odata[pos] & idata[pos];
    pos--;
    p_buffer[n--] = LUTW[diffw >> 4] & (LUTB[diffb >> 4]);
    p_buffer[n--] = LUTW[diffw & 0x0F] & (LUTB[diffb & 0x0F]);
  }
}
```

Both buffers are traversed from their last byte toward byte 0.  One input byte
produces two output bytes (upper/lower nibble each get their own LUT entry).
`n` (uint32_t) safely covers the range 230399→0.

### Transmission phase (`eink_5v2.cpp:374–403`)

```
4 waveform phases × 720 rows × (WIDTH/4 = 320 bytes/row) = 921 600 bytes total
```

```cpp
for (int k = 0; k < 4; k++) {
  uint8_t *dp = p_buffer;     // reset to start each phase
  vscan_start();

  for (int i = 0; i < HEIGHT; i++) {
    uint8_t *row_ptr = (dp + (WIDTH / 4)) - 1;  // end of this p_buffer row
    for (int j = 0; j < (WIDTH / 4); j += 4) {
      line_buffer[j + 2] = *row_ptr--;
      line_buffer[j + 3] = *row_ptr--;
      line_buffer[j]     = *row_ptr--;
      line_buffer[j + 1] = *row_ptr--;
    }
    dp += (WIDTH / 4);
    i2s_comms.send_data();
    vscan_end();
  }
  ESP::delay_microseconds(230);
}
```

Each inner iteration writes 4 bytes to `line_buffer`; with `j` stepping by 4 over
`WIDTH/4 = 320` positions, there are 80 inner iterations per row, consuming exactly
320 bytes — well within the 336-byte DMA buffer.

After all phases, `d_memory_new` is updated by `memcpy` at line 412.

---

## 4. Magic Numbers 600, 758, 825, 800, 1024 in Partial-Refresh Paths

A complete search of all files under
`components/ESP-IDF-InkPlate/src/drivers/` and
`components/ESP-IDF-InkPlate/src/services/`:

| Value | File | Line | Context |
|---|---|---|---|
| 800 | `eink_6.hpp` | 53 | `WIDTH = 800` (InkPlate 6) |
| 600 | `eink_6.hpp` | 54 | `HEIGHT = 600` (InkPlate 6) |
| 800 | `eink_6v2.hpp` | 50 | `WIDTH = 800` (InkPlate 6V2) |
| 600 | `eink_6v2.hpp` | 51 | `HEIGHT = 600` (InkPlate 6V2) |
| 1024 | `eink_6plus.hpp` | 52 | `WIDTH = 1024` (InkPlate 6PLUS) |
| 758 | `eink_6plus.hpp` | 53 | `HEIGHT = 758` (InkPlate 6PLUS) |
| 1024 | `eink_6plus_v2.hpp` | 52 | `WIDTH = 1024` (InkPlate 6PLUS V2) |
| 758 | `eink_6plus_v2.hpp` | 53 | `HEIGHT = 758` (InkPlate 6PLUS V2) |
| 1024 | `eink_6flick.hpp` | 49 | `WIDTH = 1024` (InkPlate 6FLICK) |
| 758 | `eink_6flick.hpp` | 50 | `HEIGHT = 758` (InkPlate 6FLICK) |
| 825 | `eink_10.hpp` | 57 | `HEIGHT = 825` (InkPlate 10) |

**None of these values appear anywhere in `eink_5v2.cpp` or `eink_5v2.hpp`.**  
The 5V2 driver references only its own constants (`WIDTH=1280`, `HEIGHT=720`) and
the derived values through `BITMAP_SIZE_1BIT` / `LINE_SIZE_1BIT`.

The I2S and DMA code in `i2s_comms.hpp` / `i2s_comms.cpp` contains no panel-width
literals; all sizes are passed in as `buffer_size` from the subclass constructor.

---

## 5. Comparison with EInk6V2

The 6V2 `partial_update` (DMA variant, `eink_6v2.cpp:335`) is structurally identical
to the 5V2 implementation — same fill-phase logic, same `uint16_t pos` declaration,
same `uint32_t n` declaration.

The critical difference is panel size:

| | 5V2 | 6V2 |
|---|---|---|
| `WIDTH` | 1280 | 800 |
| `HEIGHT` | 720 | 600 |
| `BITMAP_SIZE_1BIT` | **115 200** | 60 000 |
| `uint16_t` max | 65 535 | 65 535 |
| `pos = BITMAP_SIZE_1BIT - 1` | **115 199 → overflows** | 59 999 ✓ |

For the 6V2, `BITMAP_SIZE_1BIT - 1 = 59 999`, which fits in a `uint16_t`. For the
5V2, `BITMAP_SIZE_1BIT - 1 = 115 199 > 65 535`, which **does not fit**.

---

## 6. Root Cause: `uint16_t pos` Overflow

**`eink_5v2.cpp:351` — and the identical line in `eink_6v2.cpp:347`:**

```cpp
uint16_t pos = BITMAP_SIZE_1BIT - 1;
```

`BITMAP_SIZE_1BIT` is declared `uint32_t` (`eink_5v2.hpp:55`). Its value for the
5V2 is 115 200. Assigning `115 200 - 1 = 115 199` to a `uint16_t` truncates modulo
65 536:

```
115 199 % 65 536 = 49 663
```

The fill loop runs `HEIGHT × LINE_SIZE_1BIT = 720 × 160 = 115 200` iterations,
with `pos` decrementing by 1 each iteration and wrapping at 0 → 65 535 (unsigned
underflow). Tracing the actual bytes accessed:

- `pos` starts at **49 663** (not 115 199).
- Bytes **49 663 → 0**: read on iterations 1–49 664.
- `pos` wraps from 0 to **65 535**.
- Bytes **65 535 → 0**: read on iterations 49 665–115 200 (byte 0 is read a second time).

Consequence:

| Range | Status |
|---|---|
| Bytes 0–49 663 (rows 0–310) | Read during iterations 1–49 664 |
| Bytes 49 664–65 534 (rows 310–409) | **Never read** |
| Bytes 65 535 (row 409, byte 95) | Read once (wrap boundary) |
| Bytes 65 536–115 199 (rows 410–719) | **Never read — above uint16_t range** |
| Byte 0 | Read **twice** |

**49 664 bytes (rows 410–719, the top ~43% of the 720-row panel) are never
accessed.** The corresponding 99 328 bytes in `p_buffer` (two output bytes per
input byte) are computed against framebuffer byte 0 — not the actual pixel content
of those rows.

This means the waveform sent to the panel for the top 310 rows is entirely wrong
during a partial update. The bottom ~410 rows update correctly (their framebuffer
bytes are in the 0–49 663 range that `pos` does reach). This produces a horizontal
split: the lower portion of the display updates normally; the upper portion does not.

The 6V2 is unaffected because its `BITMAP_SIZE_1BIT = 60 000` fits within
`uint16_t` — the code was ported from 6V2 to 5V2 without widening the `pos` type.

---

## Summary

| Question | Finding |
|---|---|
| Full refresh on every page turn? | Yes — prior to this session's fix, `force_full_update()` reset `partial_count = 0` before each render. That has now been removed. |
| Dirty rectangle? | No. Full framebuffer diff computed and transmitted on every partial update. |
| Root cause of split-screen? | `uint16_t pos` at `eink_5v2.cpp:351` overflows for the 5V2's 115 200-byte bitmap. `pos` starts at 49 663 instead of 115 199, leaving rows 410–719 (top 43% of panel) with wrong differential data. |
| Does the bug exist in the 6V2? | No — 6V2 bitmap is 60 000 bytes; `pos = 59 999` fits in `uint16_t`. |
| Magic numbers 600/758/800/825/1024 in 5V2 code? | None. All foreign panel dimensions are isolated to their own driver headers. |
