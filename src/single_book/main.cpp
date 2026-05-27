// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.
//
// Single-book BUILD_VARIANT entry point.
//
// Replaces src/main.cpp + the full controller stack with a minimal loop
// that opens the first .epub found on SD (or LittleFS — see Phase 2),
// restores the last-read position from NVS, and maps one physical button
// (GPIO 36, active LOW) to page navigation and sleep.
//
// Storage requirements (same layout as the full firmware):
//   MAIN_FOLDER/fonts_list.xml
//   MAIN_FOLDER/fonts/          (font files declared in fonts_list.xml)
//   MAIN_FOLDER/books/*.epub    (at least one)
//
// Button gestures (GPIO 36):
//   short press  → next page
//   double press → previous page
//   long press   → status overlay (page X/Y + battery)
//   very long    → save position + deep sleep
//   idle 30 min  → auto deep sleep
//
// Wake behaviour (Phase 1):
//   Cold boot (power-on / reset / brownout): renders the saved page immediately.
//   Wake from deep sleep (ESP_SLEEP_WAKEUP_EXT0): skips the initial render —
//   the e-ink panel already shows the previous page from retention. The first
//   button press triggers the normal black/white/render sequence.
//
//   Overlay safety: if EVT_LONG fires before the first page turn on a
//   wake-from-sleep boot, show_page() is called first so the overlay
//   has a valid framebuffer to draw over (avoids blank-panel partial refresh
//   caused by partial_allowed=false after deep-sleep reinitialisation).

#define __GLOBAL__ 1
#include "global.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "models/config.hpp"
#include "models/epub.hpp"
#include "models/fonts.hpp"
#include "models/nvs_mgr.hpp"
#include "models/page_locs.hpp"
#include "viewers/book_viewer.hpp"
#include "viewers/msg_viewer.hpp"
#include "viewers/page.hpp"
#include "screen.hpp"
#include "inkplate_platform.hpp"
#include "alloc.hpp"
#include "esp.hpp"
#include "pugixml.hpp"

#include "single_book/wake_button_mgr.hpp"
#include "single_book/status_overlay.hpp"
#include "utils/book_id.hpp"

#include "esp_sleep.h"
#if !CONFIG_USE_SD_CARD
  #include "esp_littlefs.h"
#endif

extern "C" {
  #include <dirent.h>
}
#include <sys/stat.h>
#include <cstring>
#include <string>

static constexpr char const * TAG      = "single_book";
static constexpr gpio_num_t   WAKE_PIN = GPIO_NUM_36;

static constexpr uint32_t IDLE_POLL_MS          =   100;
static constexpr uint32_t DEEP_SLEEP_TIMEOUT_MS = 3 * 60 * 1000 + 30 * 1000;  // 3.5 minutes

// Returns the bare filename of the first .epub in BOOKS_FOLDER, or "".
static std::string find_first_epub()
{
  std::string result;
  DIR * dp = opendir(BOOKS_FOLDER);
  if (dp == nullptr) return result;

  struct dirent * de;
  while ((de = readdir(dp)) != nullptr) {
    int16_t len = static_cast<int16_t>(strlen(de->d_name));
    if ((len > 5) && (strcasecmp(&de->d_name[len - 5], ".epub") == 0)) {
      result = de->d_name;
      break;
    }
  }
  closedir(dp);
  return result;
}

static void persist_position(uint32_t                  book_id,
                             const PageLocs::PageId &  page_id,
                             bool                      going_to_sleep)
{
  NVSMgr::NVSData data = {
    .offset        = page_id.offset,
    .itemref_index = page_id.itemref_index,
    .was_shown     = going_to_sleep ? uint8_t(1) : uint8_t(0),
    .filler1       = 0,
  };
  nvs_mgr.save_location(book_id, data);
}

// Save position and sleep. No message is shown: the e-ink panel keeps the current
// page visible without power, so the page itself signals the sleeping state.
static void go_to_sleep(uint32_t book_id, const PageLocs::PageId & page_id)
{
  persist_position(book_id, page_id, true);
  #if !CONFIG_USE_SD_CARD
    // LittleFS has power-loss journaling but an explicit unmount flushes
    // any pending writes and is good practice before cutting power.
    esp_vfs_littlefs_unregister("storage");
  #endif
  inkplate_platform.deep_sleep(WAKE_PIN, 0);
  // not reached
}

static void show_page(const PageLocs::PageId & page_id)
{
  book_viewer.show_page(page_id);
}

static void mainTask(void * /*params*/)
{
  LOG_I("Single-book EPub reader starting.");

  bool waking = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);

  // --- Hardware init ---
  bool nvs_ok = nvs_mgr.setup();

  // sd_card_init: only initialise the SD driver stack when SD storage is in use.
  // CONFIG_USE_SD_CARD is a Kconfig bool: defined as 1 when y, NOT defined (not 0)
  // when n. Using it directly as a function argument would compile to setup() with
  // no argument when disabled. Use #ifdef to produce an explicit true/false literal.
  bool platform_ok = inkplate_platform.setup(
      /*sd_card_init=*/
#ifdef CONFIG_USE_SD_CARD
      true
#else
      false
#endif
  );
  if (!platform_ok) {
    LOG_E("InkPlate platform setup failed — restarting.");
    esp_restart();
  }

  // --- Storage mount ---
  #if !CONFIG_USE_SD_CARD
  {
    esp_vfs_littlefs_conf_t lfs_conf = {
      .base_path             = "/littlefs",
      .partition_label       = "storage",
      .format_if_mount_failed = false,   // never auto-format — content must be flashed
      .dont_mount            = false,
    };
    esp_err_t lfs_err = esp_vfs_littlefs_register(&lfs_conf);
    if (lfs_err != ESP_OK) {
      LOG_E("LittleFS mount failed (%d). Flash the storage partition: "
            "esptool.py write_flash 0x310000 build_single/storage.bin", lfs_err);
      // Can't recover without storage — sleep and wait for reflash.
      inkplate_platform.deep_sleep(WAKE_PIN, 0);
    }
  }
  #endif

  // config failure is non-fatal: font index defaults to 0 (first USER font)
  config.read();
  // show_heap and show_title are suppressed in SINGLE_BOOK_BUILD via compile-time
  // guards in screen_bottom.cpp and book_viewer.cpp so they cannot be overridden
  // here. Resolution is hardcoded ONE_BIT in screen.setup() below; orientation
  // is read from config so it stays consistent with any previously saved NVS offsets.

  pugi::set_memory_management_functions(allocate, free);

  // --- App-layer init (must match full main.cpp order) ---
  page_locs.setup();   // spawns retriever + state threads

  if (!fonts.setup()) {
    LOG_E("Font loading failed. Check " MAIN_FOLDER "/fonts_list.xml.");
    esp_restart();
  }

  // ONE_BIT: grayscale (THREE_BITS) disables partial refresh, making page turns slow.
  // Orientation is read from config (default RIGHT=1): RIGHT maps the 1280×720 panel
  // to 720×1280 portrait, which is correct when the device is held vertically.
  // This must stay consistent across boots — changing orientation invalidates NVS
  // page offsets and breaks position restore.
  // LEFT and RIGHT are both portrait (720×1280) — same page layout, same NVS offsets.
  // LEFT rotates 180° relative to RIGHT; use LEFT so the device reads naturally
  // with the USB port at the bottom.
  screen.setup(Screen::PixelResolution::ONE_BIT, Screen::Orientation::LEFT);

  if (!nvs_ok) {
    msg_viewer.show(MsgViewer::MsgType::ALERT, false, true,
      "Hardware Problem!", "NVS Flash init failed. Position will not be saved.");
    ESP::delay(3000);
  }

  // --- Locate book ---
  std::string book_fname = find_first_epub();
  if (book_fname.empty()) {
    msg_viewer.show(MsgViewer::MsgType::ALERT, false, true,
      "No Book Found",
      "Place a .epub in " BOOKS_FOLDER " then"
#if CONFIG_USE_SD_CARD
      " press WakeUp to restart."
#else
      " reflash storage partition."
#endif
      );
    ESP::delay(500);
    inkplate_platform.deep_sleep(WAKE_PIN, 0);
  }
  std::string full_path = std::string(BOOKS_FOLDER "/") + book_fname;

  // Jenkins96 of bare filename — must match what BooksDirController passes
  // to nvs_mgr so position records survive a reboot into the full firmware.
  uint32_t book_id = generate_id(
    reinterpret_cast<const uint8_t *>(book_fname.c_str()),
    static_cast<uint32_t>(book_fname.length())
  );

  // --- Restore saved position, or start from the beginning ---
  PageLocs::PageId saved_page_id(0, 0);
  if (nvs_ok) {
    NVSMgr::NVSData nvs_data;
    if (nvs_mgr.get_location(book_id, nvs_data)) {
      saved_page_id.itemref_index = nvs_data.itemref_index;
      saved_page_id.offset        = nvs_data.offset;
    }
  }

  // --- Open book ---
  // On wake from deep sleep, skip the "Loading..." splash — the e-ink panel
  // already shows the book page from before sleep, so a silent reload feels
  // seamless. On cold boot, show the splash so the user knows something is happening.
  if (!waking) {
    msg_viewer.show(MsgViewer::MsgType::BOOK, false, false,
      "Loading", "\"%s\" — please wait.", book_fname.c_str());
  }

  bool new_document = (book_fname != epub.get_current_filename());
  if (new_document) page_locs.stop_document();

  if (!epub.open_file(full_path)) {
    msg_viewer.show(MsgViewer::MsgType::ALERT, false, true,
      "Open Failed", "Cannot open \"%s\".", book_fname.c_str());
    ESP::delay(500);
    inkplate_platform.deep_sleep(WAKE_PIN, 0);
  }

  page_locs.start_new_document(epub.get_item_count(),
                                saved_page_id.itemref_index);
  book_viewer.init();

  // Blocks until the starting item is paginated (cache hit = fast;
  // cold first-open of a large book may take several seconds).
  const PageLocs::PageId * id = page_locs.get_page_id(saved_page_id);
  PageLocs::PageId current_page_id = (id != nullptr)
                                       ? *id
                                       : PageLocs::PageId(0, 0);

  // On cold boot: render the saved page immediately (panel state unknown —
  // we need at least one full waveform cycle to establish a clean baseline).
  // On wake from deep sleep: skip — the panel already shows the correct page
  // from e-ink retention. The first button press will do the normal
  // black/white/render sequence, which is the right moment for a waveform cycle.
  bool rendered = false;
  if (!waking) {
    show_page(current_page_id);
    rendered = true;
  }

  // --- Button manager ---
  WakeButtonMgr buttons(WAKE_PIN);
  buttons.start();

  int64_t last_activity_ms = ESP::millis();

  // --- Main event loop ---
  while (true) {
    ButtonEvent evt = buttons.wait_for_event_timeout(IDLE_POLL_MS);

    switch (evt) {

      case ButtonEvent::EVT_SHORT: {
        const PageLocs::PageId * next =
          page_locs.get_next_page_id(current_page_id);
        if (next != nullptr) {
          current_page_id = *next;
          show_page(current_page_id);
          rendered = true;
          persist_position(book_id, current_page_id, false);
        }
        last_activity_ms = ESP::millis();
        break;
      }

      case ButtonEvent::EVT_DOUBLE: {
        const PageLocs::PageId * prev =
          page_locs.get_prev_page_id(current_page_id);
        if (prev != nullptr) {
          current_page_id = *prev;
          show_page(current_page_id);
          rendered = true;
          persist_position(book_id, current_page_id, false);
        }
        last_activity_ms = ESP::millis();
        break;
      }

      case ButtonEvent::EVT_LONG:
        // If the overlay fires before the first page render on a wake-from-sleep
        // boot, partial_allowed is still false (reset by deep-sleep reinit) and
        // the partial_update call inside ScreenBottom::show() would fall back to
        // e_ink::update() with an empty framebuffer — producing a blank panel.
        // Render the current page first so the overlay has something to draw over.
        if (!rendered) {
          show_page(current_page_id);
          rendered = true;
        }
        StatusOverlay::show(current_page_id);
        last_activity_ms = ESP::millis();
        break;

      case ButtonEvent::EVT_VLONG:
        go_to_sleep(book_id, current_page_id);
        break;  // not reached

      case ButtonEvent::EVT_NONE:
      default:
        break;
    }

    if (ESP::millis() - last_activity_ms >
        static_cast<int64_t>(DEEP_SLEEP_TIMEOUT_MS)) {
      go_to_sleep(book_id, current_page_id);
    }
  }
}

static constexpr uint32_t STACK_SIZE = 60000;

extern "C" {
  void app_main(void)
  {
    TaskHandle_t handle = nullptr;
    xTaskCreate(mainTask, "mainTask", STACK_SIZE, nullptr,
                configMAX_PRIORITIES - 1, &handle);
    configASSERT(handle);
  }
}
