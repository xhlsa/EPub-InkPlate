# EPub-InkPlate → Inkplate 5Gen2 Port Notes

Phase 0 reconnaissance. Read this before writing a single line of code.

---

## Status: Germain's 5V2 work confirmed — submodule pin needs updating

The plan's premise was correct. Germain Masse's work is in the library as PR #28 (`gmasse/5v2`),
merged to `origin/master`. The issue was that the EPub-InkPlate repo had the submodule pinned to
commit `64200a6` (January 2026), before the merge. After running `git fetch && git checkout
origin/master` inside the submodule, it is now at `d9770a9`.

**The parent repo's submodule pin must be updated** before committing anything else:
```
# from EPub-InkPlate/ root:
git add components/ESP-IDF-InkPlate
git commit -m "submodule: advance ESP-IDF-InkPlate to d9770a9 (adds INKPLATE_5V2 and INKPLATE_6V2)"
```

**Devices now in the library (origin/master d9770a9):**

| Define | IO Expander | EInk class | IOExp addrs | Data bus |
|--------|-------------|------------|-------------|----------|
| `INKPLATE_6` | MCP23017 | `EInk6` | 0x20 | parallel GPIO |
| `INKPLATE_5V2` | PCAL6416 | `EInk5V2` | 0x20 only | I2S/DMA |
| `INKPLATE_6V2` | PCAL6416 | `EInk6V2` | 0x20 only | I2S/DMA |
| `INKPLATE_6PLUS` | MCP23017 | `EInk6PLUS` | 0x20, 0x22 | parallel GPIO |
| `INKPLATE_6PLUS_V2` | PCAL6416 | `EInk6PLUSV2` | 0x20, 0x21 | parallel GPIO |
| `INKPLATE_6FLICK` | PCAL6416 | `EInk6FLICK` | 0x20, 0x21 | I2S/DMA |
| `INKPLATE_10` | MCP23017 | `EInk10` | 0x20, 0x22 | parallel GPIO |

---

## 1. Abstraction layer between app and library

**Assessment: Clean and well-structured.**

The device type flows in exactly one direction:

```
idf.py build -DDEVICE=INKPLATE_6PLUS_V2
    ↓
CMakeLists.txt (top-level):
    add_compile_definitions(INKPLATE_6PLUS_V2=1  PCAL6416=1)
    ↓
Library: #if INKPLATE_6PLUS_V2 guards throughout
App: #if INKPLATE_6PLUS_V2 || INKPLATE_6PLUS || INKPLATE_6FLICK guards
```

No device identity leaks through runtime polymorphism. Everything is resolved at compile time.
The central wiring file is `components/ESP-IDF-InkPlate/src/drivers/inkplate_platform.hpp`,
which instantiates all hardware singletons (`io_expander_int`, `battery`, `sd_card`,
`e_ink`, `rtc`) based on which define is active.

### Files that must be touched to add INKPLATE_5_V2

**In the library** (`components/ESP-IDF-InkPlate/`):
- `src/drivers/eink_5v2.hpp` — new file: `EInk5V2` class (copy `eink_6plus_v2.hpp`, change dims)
- `src/drivers/eink_5v2.cpp` — new file: display driver implementation (pin map from schematic)
- `src/drivers/inkplate_platform.hpp` — add `#include "eink_5v2.hpp"` and the `INKPLATE_5_V2`
  instantiation branch at lines 77-84 (currently ends in `#error`)
- `src/graphical/inkplate.hpp` — add `INKPLATE_5_V2` to the `begin()` overload selector and
  any method guards that currently list `INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK`

**In the app** (`EPub-InkPlate/`):
- `CMakeLists.txt` — add `INKPLATE_5_V2` block (lines 26-31 pattern)
- `components/inkplate_screen/src/screen.hpp` — add `INKPLATE_5_V2` to the `IDENT`/`RESOLUTION`
  block at lines 26-38
- `src/main.cpp` — add to `INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK` guards
  at lines 28-29, 68, 120, 252
- `src/controllers/touch_event_mgr.cpp` — add to guard at line 9 (whole file is conditional)
- `src/controllers/event_mgr.cpp` — `INKPLATE_5_V2` does NOT use this file (it's for devices
  with MCP23017 touch pads); the event manager for 5V2 will live in `touch_event_mgr.cpp`
  since it's the 6PLUS_V2 pattern but with button polling instead of touch screen
- `src/controllers/common_actions.cpp` — add to INKPLATE_6PLUS guards at lines 49-50
- `src/viewers/msg_viewer.cpp` — add to INKPLATE_6PLUS guards at lines 353-354

---

## 2. Where INKPLATE_5V2 fits relative to existing devices

**Define name: `INKPLATE_5V2`** — no underscores between 5 and V2, no "GEN2".
This is the canonical name in the library's `SetupApp.txt` and all driver guards.

The 5V2 is structurally closer to `INKPLATE_6V2` than to `INKPLATE_6PLUS_V2`:
- Single IOExpander at 0x20 (no ext at 0x21 like 6PLUS_V2 uses)
- I2S/DMA data bus (like 6V2 and 6FLICK, NOT like 6PLUS_V2's parallel GPIO)
- No touch screen, no touchpad — the library defines no input handling for 5V2
- `PCAL6416` IO expander ✓
- SD power on `IOPIN_10` (not `IOPIN_13` like 6PLUS_V2)
- I2C **internal pull-ups must be enabled** (`wire.cpp:29-32`) — the 5V2 board has no external
  pull-up resistors on SDA/SCL

Key differences from Phase 0 assumptions:
- No ext IO expander → EInk5V2 constructor takes ONE IOExpander arg, not two
- I2S init call: `i2s_comms.init(8)` (8-bit width)
- The `touch_event_mgr.cpp` event loop (used by 6PLUS/6PLUS_V2/6FLICK) is NOT automatically
  the right home for 5V2 input handling; the 5V2 has no touch screen at all

---

## 3. Magic numbers — screen dimensions

**None found.** The grep for `600|758|800|825|1024|1200|1280|720` across `src/` and `include/`
returned only non-display values: stack sizes, max file sizes, font buffer limits.

Screen dimensions flow through:
```
EInk5V2::WIDTH / HEIGHT (new constants, 1280 / 720)
    ↓ e_ink.get_width() / e_ink.get_height()
    ↓ Screen::width / Screen::height  (set in screen.cpp setup())
    ↓ Screen::get_width() / get_height()  (used everywhere in viewers/)
```

The layout engine is fully parametric on screen size. No porting work needed here beyond
defining correct dimensions in the new EInk class.

One non-trivial geometry difference: the 5Gen2 at 1280×720 is 16:9 landscape, while the
6PLUS_V2 at 1024×758 is approximately portrait-4:3. The EPub layout engine renders text into
`screen.get_width()` columns and `screen.get_height()` rows, so it will adapt automatically.
However the `Screen::RESOLUTION` constant (pixels per inch) in `screen.hpp:37` is used for
font sizing and must be set correctly for the 5Gen2 panel. The 6PLUS_V2 is 212 PPI. Calculate
5Gen2 PPI from the panel's physical dimensions in the datasheet.

---

## 4. Wake button code path

**File:** `src/controllers/touch_event_mgr.cpp` (only compiled for INKPLATE_6PLUS variants)

### Sleep entry (`touch_event_mgr.cpp:706-733`)

```
EventMgr::loop() — runs continuously in mainTask
    if (no event in 15 seconds && !stay_on):
        inkplate_platform.light_sleep(duration_minutes, TouchScreen::INTERRUPT_PIN, 0)
        if (light_sleep returns true — timed out):
            app_controller.going_to_deep_sleep()
            inkplate_platform.deep_sleep(TouchScreen::INTERRUPT_PIN, 0)
```

The GPIO used for both light sleep and deep sleep wakeup is `TouchScreen::INTERRUPT_PIN`
= `GPIO_NUM_36` (defined in `touch_screen_elan.hpp:27`).

For the 5Gen2, there is no ELAN touch screen, so this GPIO reference must be replaced with a
new constant specific to the 5Gen2 WAKE button pin. **Do not assume GPIO 36.** Read the
Soldered Inkplate 5Gen2 schematic.

### Wake button event (`touch_event_mgr.cpp` and `app_controller.cpp:115`)

The event `EventKind::WAKEUP_BUTTON` is emitted when the GPIO fires. In the app:
```
app_controller.cpp:115:
    else if (event.kind == EventMgr::EventKind::WAKEUP_BUTTON)
```
This is where action bindings for a single-button mode will hook in (Phase 4/5).

### Deep sleep configuration

`InkPlatePlatform::deep_sleep(gpio_num_t gpio_num, int level)` calls
`esp_sleep_enable_ext0_wakeup(gpio_num, level)`. For the 5Gen2, `level=0` if the wake button
pulls the GPIO low when pressed (active-low, the common case). Verify from schematic.

---

## 5. Hardware unknowns — schematic required before writing code

These cannot be assumed from the 6PLUS_V2 and must be read from the Soldered Inkplate 5Gen2
schematic before writing any driver code:

| Unknown | 6PLUS_V2 value | Why it might differ |
|---------|---------------|---------------------|
| WAKE button GPIO | GPIO_NUM_36 | 5Gen2 routing differs |
| SD MISO | GPIO_NUM_12 | May share SPI with display or differ |
| SD MOSI | GPIO_NUM_13 | Same concern |
| SD CLK | GPIO_NUM_14 | Same concern |
| SD CS | GPIO_NUM_15 | Same concern |
| SD power IOPIN | IOPIN_13 | May be different expander pin |
| PCAL6416 int addr | 0x20 | Address strapping may differ |
| PCAL6416 ext addr | 0x21 | Same |
| Display data GPIOs (D0-D7) | 4,5,18,19,23,25,26,27 | May differ |
| Display control GPIOs | 0,2,32,33 (CKV=33,SPH=34,LE=2) | May differ |
| PWRMGR I2C addr | 0x48 (TPS65185) | 5Gen2 may use different PMIC |
| IO expander OE/GMOD/SPV/WAKEUP/PWRUP/VCOM IOPIN | 0,1,2,3,4,5 | May differ |

Also confirm: the 5Gen2 board does use PCAL6416A specifically (not a different I/O expander),
by visually inspecting the chip package on the actual board and cross-referencing the markings
with the schematic.

---

## 6. SD card pins

Defined in `components/ESP-IDF-InkPlate/src/drivers/sd_card.hpp:38-41`:

```cpp
static constexpr gpio_num_t PIN_NUM_MISO = GPIO_NUM_12;
static constexpr gpio_num_t PIN_NUM_MOSI = GPIO_NUM_13;
static constexpr gpio_num_t PIN_NUM_CLK  = GPIO_NUM_14;
static constexpr gpio_num_t PIN_NUM_CS   = GPIO_NUM_15;
```

These are not guarded by device — they are the same for all current devices. If the 5Gen2
uses different SPI pins for the SD slot, add a `#if INKPLATE_5_V2` block here.

The SD power pin (PCAL6416 `IOPIN_13`) is guarded:
```cpp
// sd_card.hpp:34-36
#if INKPLATE_6PLUS_V2 || INKPLATE_6FLICK
  static constexpr IOExpander::Pin SD_POWER = IOExpander::Pin::IOPIN_13;
#endif
```
Verify this IOPIN assignment from schematic before adding `INKPLATE_5_V2` to this guard.

---

## 7. Examples

`components/ESP-IDF-InkPlate/examples/Basic_Inkplate_Functionality/` contains:
- `Inkplate_basic_BW`
- `Inkplate-basic_custom_font`
- `Inkplate_basic_gray`
- `Inkplate-basic_partial_update`

These examples use `Inkplate display(DisplayMode::INKPLATE_1BIT)` and call
`display.begin()` / `display.clearDisplay()` / `display.display()`. They are device-agnostic
at the API level and select the device via compile-time defines. There is **no Inkplate 5Gen2
hello-world example.** The BW example's `CMakeLists.txt` selects the target from
`sdkconfig.defaults`.

For Phase 1, adapt `Inkplate_basic_BW` by adding `INKPLATE_5_V2` support to the library
first, then using that example to validate the driver in isolation before touching the app.

---

## 8. Partition and sdkconfig

`partitions.csv`:
```
factory, 0, 0, 0x10000, 0x250000   (2,359,296 bytes = ~2.25MB)
```
This matches the 4MB flash ESP32-WROVER. No change needed for 5Gen2.

`sdkconfig.defaults`: Configures SPIRAM (PSRAM), 240MHz CPU, 4MB flash, custom partition table.
All values are compatible with the 5Gen2's ESP32-WROVER. No device-specific values present.

---

## 9. Canonical naming — confirmed

| Item | Value |
|------|-------|
| CMake define | `INKPLATE_5V2` |
| Compile flag | `INKPLATE_5V2=1  PCAL6416=1` |
| EInk class | `EInk5V2` |
| Build label | `I5V2` (follow existing pattern) |

**No `DMA_ENABLE` needed** — unlike `INKPLATE_6V2` (which requires `DMA_ENABLE=1`), the 5V2
driver always uses I2S; the `#if` guard in `eink.hpp` already includes `INKPLATE_5V2`
unconditionally: `#if ... || INKPLATE_5V2 || ...`.

---

## 10. Known library bug to be aware of

`inkplate_platform.hpp` has a duplicate `#elif INKPLATE_5V2` (lines 56-57 in the `#if
__INKPLATE_PLATFORM__` block, and lines 95-96 in the `extern` block). The second occurrence is
dead code — it can never be reached because the first `#if INKPLATE_5V2` at line 52 already
catches it. This is a cosmetic bug and does not affect correctness. Do not "fix" it in a port
PR without turgu1's awareness; it belongs in a separate upstream fix.

---

## 11. Wake button GPIO — still unresolved

The library defines no `INTERRUPT_PIN` or `WAKE_BUTTON_GPIO` for `INKPLATE_5V2`. The driver
handles no input at all; that is entirely the app's responsibility.

The schematic PDF `doc/Schematics/Soldered Inkplate 5 V2.pdf` is now in the submodule but
requires poppler to render. The GPIO must be determined before Phase 2's `deep_sleep()` call
can be correctly configured. Candidates based on ESP32 RTC-capable GPIOs typically used for
wake: 34, 35, 36, 39 (all input-only, RTC-capable). **Read the schematic before assuming any
of these.**

**Do not proceed past Phase 2 without confirming the wake GPIO from the schematic.**
