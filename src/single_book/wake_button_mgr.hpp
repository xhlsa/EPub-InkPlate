// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#pragma once

#include "global.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

enum class ButtonEvent : uint8_t {
  EVT_NONE   = 0,
  EVT_SHORT,    // single tap < LONG_MS, no second press within DOUBLE_GAP_MS
  EVT_DOUBLE,   // second press within DOUBLE_GAP_MS of first release
  EVT_LONG,     // hold >= LONG_MS, released before VLONG_MS
  EVT_VLONG,    // hold >= VLONG_MS, emitted while button is still held
};

// Polling button state machine for a single active-LOW GPIO.
// Spawns a FreeRTOS task; caller receives classified events via
// wait_for_event_timeout().
class WakeButtonMgr {
public:
  explicit WakeButtonMgr(gpio_num_t pin);

  // Spawn the background polling task. Call once after construction.
  void start();

  // Block up to timeout_ms waiting for a classified event.
  // Returns EVT_NONE on timeout.
  ButtonEvent wait_for_event_timeout(uint32_t timeout_ms);

private:
  static constexpr uint16_t DEBOUNCE_MS   =   20;
  static constexpr uint16_t DOUBLE_GAP_MS =  280;
  static constexpr uint16_t LONG_MS       =  600;
  static constexpr uint16_t VLONG_MS      = 2500;
  static constexpr uint16_t POLL_MS       =    5;
  static constexpr uint32_t TASK_STACK    = 2048;

  enum class State : uint8_t {
    IDLE,
    DEBOUNCE_DOWN,  // saw first falling edge, waiting for debounce
    DOWN,           // confirmed pressed, timing hold duration
    DEBOUNCE_UP,    // saw rising edge after short press, waiting debounce
    WAIT_SECOND,    // released after short press, waiting for double-tap window
    DRAIN,          // after LONG/VLONG/DOUBLE: ignore until button fully released
  };

  gpio_num_t    pin_;
  QueueHandle_t event_queue_;
  State         state_;
  int64_t       t_edge_ms_;   // timestamp of last state-triggering edge (ms)

  static void task_fn(void * arg);
  void        task_body();
  void        push(ButtonEvent evt);
};
