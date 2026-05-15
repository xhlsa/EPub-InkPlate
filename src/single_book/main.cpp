// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.
//
// Single-book BUILD_VARIANT entry point.
//
// Replaces src/main.cpp + the full controller stack with a minimal loop
// that opens the first .epub found on the SD card, restores the last-read
// position from NVS, and maps one physical button (GPIO 36, active LOW)
// to page navigation and sleep.
//
// SD card requirements (same layout as the full firmware):
//   /sdcard/fonts_list.xml
//   /sdcard/fonts/          (font files declared in fonts_list.xml)
//   /sdcard/books/*.epub    (at least one)
//
// Button gestures (GPIO 36):
//   short press  → next page
//   double press → previous page
//   long press   → status overlay (page X/Y + battery)
//   very long    → save position + deep sleep
//   idle 30 min  → auto deep sleep
//
// Sleep behaviour: no "going to sleep" message is shown — the e-ink panel
// retains the last book page without power, so the page IS the sleep indicator.
// On wake the device re-renders the same page from NVS; cold boots show a
// brief "Loading..." splash, wake-from-sleep skips it.

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

extern "C" {
  #include <dirent.h>
}
#include <sys/stat.h>
#include <cstring>
#include <string>

static constexpr char const * TAG      = "single_book";
static constexpr gpio_num_t   WAKE_PIN = GPIO_NUM_36;

static constexpr uint32_t IDLE_POLL_MS          =   100;
static constexpr uint32_t DEEP_SLEEP_TIMEOUT_MS = 30 * 60 * 1000;  // 30 minutes

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
  inkplate_platform.deep_sleep(WAKE_PIN, 0);
  // not reached
}

// Render page with a guaranteed full refresh to avoid partial-update ghost artifacts.
static void show_page_full(const PageLocs::PageId & page_id)
{
  screen.force_full_update();
  book_viewer.show_page(page_id);
}

static void mainTask(void * /*params*/)
{
  LOG_I("Single-book EPub reader starting.");

  bool waking = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);

  // --- Hardware init ---
  bool nvs_ok = nvs_mgr.setup();

  bool platform_ok = inkplate_platform.setup(/*sd_card_init=*/true);
  if (!platform_ok) {
    LOG_E("InkPlate platform setup failed — restarting.");
    esp_restart();
  }

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
    LOG_E("Font loading failed. Check /sdcard/fonts_list.xml.");
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
      "Place a .epub file in /sdcard/books/ then press WakeUp to restart.");
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

  // Initial render — partial_count is 0 after screen.setup(), so this is already
  // a full refresh. force_full_update() is redundant here but kept for clarity.
  show_page_full(current_page_id);

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
          show_page_full(current_page_id);
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
          show_page_full(current_page_id);
          persist_position(book_id, current_page_id, false);
        }
        last_activity_ms = ESP::millis();
        break;
      }

      case ButtonEvent::EVT_LONG:
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
