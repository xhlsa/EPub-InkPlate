# InkPlate 5 Gen 2 (5V2) Porting Steps

This document records all changes made to port EPub-InkPlate to the InkPlate 5 Gen 2 device.

## Overview

The InkPlate 5 Gen 2 is a button-only device (no touchscreen) but shares architecture with touchscreen InkPlate devices. It uses:
- PCAL6416 IO expander (single expander at I2C address 0x20)
- I2S/DMA data bus for display
- 1280×720 e-ink display (284 PPI)
- Physical buttons instead of touchscreen
- ESP32 microcontroller

## Build Success Status

✅ Firmware builds successfully for INKPLATE_5V2
✅ All compilation errors resolved
⚠️  Button polling not yet implemented (generates NONE events)
⚠️  Wake button GPIO needs verification from schematic (using GPIO_NUM_36 placeholder)

## Files Modified

### 1. CMakeLists.txt
**Purpose:** Add INKPLATE_5V2 as recognized device target

```cmake
elseif(DEVICE MATCHES "^INKPLATE_5V2$")
    message("INKPLATE 5 V2 defined")
    set(DEVICE_NAME "I5V2")
    add_compile_definitions(
        INKPLATE_5V2=1
        PCAL6416=1)
```

**Location:** Lines added between INKPLATE_6 and INKPLATE_6PLUS blocks

---

### 2. components/inkplate_screen/src/screen.hpp
**Purpose:** Define screen constants for 5V2

**Changes:**
```cpp
#elif INKPLATE_5V2
  static constexpr int8_t   IDENT                 =   4;
  static constexpr int16_t  PARTIAL_COUNT_ALLOWED =  10;
  static constexpr uint16_t RESOLUTION            = 284;  ///< Pixels per inch
```

**Location:** Added before `#elif INKPLATE_6PLUS` block (around line 42)

**Also updated to_user_coord guard:**
```cpp
#if INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK || INKPLATE_5V2
  void to_user_coord(uint16_t & x, uint16_t & y);
#endif
```

---

### 3. src/controllers/common_actions.cpp
**Purpose:** Add wake button GPIO placeholder for power-off functionality

**Changes:**
```cpp
#elif INKPLATE_5V2
  #define MSG "Please press the WakeUp Button to restart the device."
  #define INT_PIN GPIO_NUM_36  // TODO: Verify from schematic - placeholder value
  #define LEVEL 0
```

**Location:** In `power_it_off()` function, around line 52

**Note:** GPIO_NUM_36 is a placeholder. Actual wake button GPIO must be verified from hardware schematic.

---

### 4. src/main.cpp
**Purpose:** Add wake button GPIO for startup error handling

**Changes:**
```cpp
#elif INKPLATE_5V2
  #define MSG "Press the WakeUp Button to restart."
  #define INT_PIN GPIO_NUM_36  // TODO: Verify from schematic
  #define LEVEL 0
```

**Location:** In `mainTask()` function, around line 72

**Same Note:** GPIO_NUM_36 placeholder needs verification.

---

### 5. src/viewers/msg_viewer.cpp
**Purpose:** Add wake button GPIO for out-of-memory handler

**Changes:**
```cpp
#elif INKPLATE_5V2
  #define MSG "Press the WakeUp Button to restart."
  #define INT_PIN GPIO_NUM_36  // TODO: Verify from schematic
  #define LEVEL 0
```

**Location:** In `out_of_memory()` function, around line 356

---

### 6. src/controllers/event_mgr.hpp
**Purpose:** Configure event system to use touchscreen-style Event structure (with x, y, dist fields) but with button-specific methods

**Changes:**

1. Added INKPLATE_5V2 include:
```cpp
#if INKPLATE_5V2
  #include "inkplate_platform.hpp"
#endif
```

2. Added INKPLATE_5V2 to touchscreen Event structure guards:
```cpp
#if INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK || INKPLATE_5V2 || TOUCH_TRIAL
  enum class EventKind { NONE, TAP, HOLD, SWIPE_LEFT,
                         SWIPE_RIGHT, PINCH_ENLARGE, PINCH_REDUCE, RELEASE,
                         WAKEUP_BUTTON};

  struct Event {
    EventKind kind;
    uint16_t x, y, dist;  // Unused for 5V2 button events
  };
```

3. Made calibration methods conditional:
```cpp
#if !INKPLATE_5V2
  void show_calibration();
  bool calibration_event(const Event & event);
  void to_user_coord(uint16_t & x, uint16_t & y);
#else
  // INKPLATE_5V2: Stub calibration methods (not used for button-only device)
  void show_calibration();
  bool calibration_event(const Event & event);
  void to_user_coord(uint16_t & x, uint16_t & y);
#endif
```

**Rationale:** INKPLATE_5V2 uses touch_event_mgr.cpp architecture (not event_mgr.cpp), so it needs the touchscreen Event structure even though x/y/dist fields will be unused for button events.

---

### 7. src/controllers/touch_event_mgr.cpp
**Purpose:** Add INKPLATE_5V2 to compilation and create minimal button event handling

**Major Changes:**

1. **Guarded touchscreen includes:**
```cpp
#if !INKPLATE_5V2
  #if INKPLATE_6FLICK
    #include "touch_screen_cypress.hpp"
  #else
    #include "touch_screen_elan.hpp"
  #endif
#endif
```

2. **Created separate event queues:**
```cpp
#if !INKPLATE_5V2
  static QueueHandle_t touchscreen_isr_queue   = NULL;
  static QueueHandle_t touchscreen_event_queue = NULL;
#else
  // INKPLATE_5V2: Button-based event queue
  static QueueHandle_t button_event_queue = NULL;
#endif
```

3. **Guarded touchscreen ISR handler:**
```cpp
#if !INKPLATE_5V2
  static void IRAM_ATTR
  touchscreen_isr_handler(void * arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(touchscreen_isr_queue, &gpio_num, NULL);
  }
#endif
```

4. **Created separate get_event_task implementations:**

Touchscreen version (existing):
```cpp
#if !INKPLATE_5V2
  #define DISTANCE  (sqrt(pow(x_end - x_start, 2) + pow(y_end - y_start, 2)))
  #define DISTANCE2 (sqrt(pow(x[1] - x[0], 2) + pow(y[1] - y[0], 2)))

  void get_event_task(void * param) {
    // ... touchscreen gesture detection state machine ...
  }
```

Button version (minimal stub for 5V2):
```cpp
#else
  // INKPLATE_5V2: Simple button polling task
  void get_event_task(void * param) {
    static constexpr char const * TAG = "GetEventTask";
    EventMgr::Event event;

    // TODO: Implement button polling via PCAL6416 IO expander
    // For now, just wait and return NONE events
    while (true) {
      event.kind = EventMgr::EventKind::NONE;
      event.x    = 0;
      event.y    = 0;
      event.dist = 0;

      // Wait for 15 seconds before returning NONE
      vTaskDelay(pdMS_TO_TICKS(15E3));
      xQueueSend(button_event_queue, &event, 0);
      taskYIELD();
    }
  }
#endif
```

5. **Updated get_event() to use correct queue:**
```cpp
const EventMgr::Event & EventMgr::get_event() {
  static Event event;
  #if !INKPLATE_5V2
    if (!xQueueReceive(touchscreen_event_queue, &event, pdMS_TO_TICKS(15E3))) {
      event.kind = EventKind::NONE;
    }
  #else
    if (!xQueueReceive(button_event_queue, &event, pdMS_TO_TICKS(15E3))) {
      event.kind = EventKind::NONE;
    }
  #endif
  return event;
}
```

6. **Guarded calibration methods and added stubs:**
```cpp
#if !INKPLATE_5V2
  void EventMgr::show_calibration() { /* touchscreen calibration */ }
  void EventMgr::to_user_coord(...) { /* coordinate transformation */ }
  bool EventMgr::calibration_event(...) { /* calibration logic */ }
  void EventMgr::retrieve_calibration_values() { /* load from config */ }
#else
  // INKPLATE_5V2: No touchscreen calibration needed for button-only device
  void EventMgr::show_calibration() {}
  void EventMgr::to_user_coord(uint16_t & x, uint16_t & y) {}
  bool EventMgr::calibration_event(const Event & event) { return false; }
#endif
```

7. **Fixed loop() wake pin references:**
```cpp
#if INKPLATE_5V2
  #define WAKE_PIN GPIO_NUM_36  // TODO: Verify from schematic
  #define WAKE_LEVEL 0
#else
  #define WAKE_PIN TouchScreen::INTERRUPT_PIN
  #define WAKE_LEVEL 0
#endif

if (inkplate_platform.light_sleep(light_sleep_duration, WAKE_PIN, WAKE_LEVEL)) {
  // ...
  inkplate_platform.deep_sleep(WAKE_PIN, WAKE_LEVEL);
}
```

8. **Updated setup() for button queue:**
```cpp
bool EventMgr::setup() {
  #if EPUB_LINUX_BUILD
    // Linux/GTK setup...
  #else
    #if !INKPLATE_5V2
      retrieve_calibration_values();
      touchscreen_isr_queue = xQueueCreate(20, sizeof(uint32_t));
      touchscreen_event_queue = xQueueCreate(20, sizeof(EventMgr::Event));
      touch_screen.set_app_isr_handler(touchscreen_isr_handler);
      TaskHandle_t xHandle = NULL;
      xTaskCreate(get_event_task, "GetEvent", 2000, nullptr, 10, &xHandle);
    #else
      // INKPLATE_5V2: Button-based event handling
      button_event_queue = xQueueCreate(20, sizeof(EventMgr::Event));
      TaskHandle_t xHandle = NULL;
      xTaskCreate(get_event_task, "GetEvent", 2000, nullptr, 10, &xHandle);
    #endif
  #endif
  return true;
}
```

---

### 8. Viewer Files (batch update)
**Files updated:** All .cpp and .hpp files in `src/viewers/`

**Purpose:** Change conditional compilation guards from touchscreen-only devices to include INKPLATE_5V2

**Pattern changed:**
```cpp
// OLD
#if INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK || TOUCH_TRIAL

// NEW
#if INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK || INKPLATE_5V2 || TOUCH_TRIAL
```

**Files affected:**
- form_viewer.cpp
- form_viewer.hpp
- keypad_viewer.hpp
- linear_books_dir_viewer.cpp
- matrix_books_dir_viewer.cpp
- menu_viewer.cpp
- menu_viewer.hpp
- msg_viewer.cpp
- msg_viewer.hpp
- toc_viewer.cpp

**Method:** Used sed to batch replace:
```bash
cd src/viewers
sed -i 's/INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK || TOUCH_TRIAL/INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK || INKPLATE_5V2 || TOUCH_TRIAL/g' *.cpp *.hpp
```

---

### 9. Controller Files (batch update)
**Files updated:** All .cpp and .hpp files in `src/controllers/`

**Purpose:** Same as viewer files - add INKPLATE_5V2 to touchscreen code paths

**Same pattern change:** Added `|| INKPLATE_5V2` to guards

**Files affected:**
- app_controller.cpp
- book_controller.cpp
- books_dir_controller.cpp
- toc_controller.cpp
- book_param_controller.cpp
- option_controller.cpp
- (and corresponding .hpp files)

**Method:** Used sed to batch replace:
```bash
cd src/controllers
sed -i 's/INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK || TOUCH_TRIAL/INKPLATE_6PLUS || INKPLATE_6PLUS_V2 || INKPLATE_6FLICK || INKPLATE_5V2 || TOUCH_TRIAL/g' *.cpp *.hpp
```

---

## Build Instructions

1. **Set device target:**
```bash
export DEVICE=INKPLATE_5V2
```

2. **Activate ESP-IDF:**
```bash
. ~/esp/v5.5.2/export.sh
```

3. **Build firmware:**
```bash
idf.py build
```

4. **Flash to device:**
```bash
idf.py -p /dev/ttyUSB0 flash
```

---

## TODO Items

### Critical (Blocking Device Functionality)
1. **Implement button polling in get_event_task()**
   - Read button states from PCAL6416 IO expander via I2C
   - Map button presses to appropriate EventKind values
   - Set up interrupt from PCAL6416 to ESP32 for efficient polling
   - Reference: ESP-IDF-InkPlate library PCAL6416 driver

2. **Verify wake button GPIO**
   - Current placeholder: GPIO_NUM_36
   - Check InkPlate 5 Gen 2 schematic for actual wake button GPIO
   - Update all 3 locations: common_actions.cpp, main.cpp, msg_viewer.cpp, touch_event_mgr.cpp

### Nice to Have
3. **Map button events to appropriate EventKind**
   - Decide button→event mapping (e.g., NEXT button → SWIPE_RIGHT)
   - Or consider adding button-specific EventKind values to enum

4. **Test on actual hardware**
   - Verify display works correctly
   - Test button input
   - Test power management (deep sleep/wake)
   - Test SD card access

5. **Optimize event polling**
   - Currently uses 15-second timeout
   - Should use interrupt-driven approach for better power efficiency

---

## Architecture Notes

### Why INKPLATE_5V2 Uses touch_event_mgr.cpp

The codebase has two event manager implementations:
1. **event_mgr.cpp:** For MCP23017-based devices with touchpad buttons
2. **touch_event_mgr.cpp:** For touchscreen devices with gesture detection

INKPLATE_5V2 uses touch_event_mgr.cpp because:
- It's already structured for FreeRTOS task-based event polling
- Has better separation between event generation and handling
- Provides the Event structure that viewer/controller code expects
- Easier to extend for button polling than adapting event_mgr.cpp

### Event Structure Design Choice

INKPLATE_5V2 uses the touchscreen Event structure (with x, y, dist fields) even though it's a button-only device:

**Pros:**
- Minimal changes to viewer/controller code
- Allows future hybrid button+touch devices
- x/y fields simply remain 0 for button events

**Cons:**
- Wastes 6 bytes per event (x, y, dist unused)
- Conceptually confusing (button events with coordinates)

**Alternative:** Create unified Event structure or button-specific subclass

---

## Compilation Summary

**Total files modified:** 18
- 1 CMakeLists.txt
- 1 screen.hpp
- 4 .cpp files (common_actions, main, msg_viewer, touch_event_mgr)
- 1 event_mgr.hpp
- ~10 viewer files (batch update)
- ~8 controller files (batch update)

**Build result:** ✅ Success
**Binary size:** Check build output for exact size
**Warnings:** 1 unused variable (non-critical)

---

## Testing Checklist

- [ ] Flash firmware to INKPLATE_5V2
- [ ] Device boots and shows startup message
- [ ] SD card mounts and fonts load
- [ ] Books list displays
- [ ] Button presses generate events (once implemented)
- [ ] Book opens and renders correctly
- [ ] Page navigation works
- [ ] Settings menu accessible
- [ ] Power off works
- [ ] Wake from deep sleep works
- [ ] WiFi features work (if enabled)

---

## References

- PORT_NOTES.md - Original porting requirements
- ESP-IDF-InkPlate library commit d9770a9 - Contains INKPLATE_5V2 display driver
- InkPlate 5 Gen 2 schematic - For wake button GPIO verification

---

**Last Updated:** 2025-05-13
**Build Status:** ✅ Compiles successfully
**Functional Status:** ⚠️ Needs button polling implementation
