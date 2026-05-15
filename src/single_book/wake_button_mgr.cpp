// Copyright (c) 2020 Guy Turcotte
//
// MIT License. Look at file licenses.txt for details.

#include "single_book/wake_button_mgr.hpp"

#include "esp_timer.h"

WakeButtonMgr::WakeButtonMgr(gpio_num_t pin)
  : pin_(pin),
    event_queue_(xQueueCreate(4, sizeof(ButtonEvent))),
    state_(State::IDLE),
    t_edge_ms_(0)
{
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = (1ULL << pin);
  cfg.mode         = GPIO_MODE_INPUT;
  cfg.pull_up_en   = GPIO_PULLUP_DISABLE;   // external pull-up on GPIO 36
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type    = GPIO_INTR_DISABLE;     // polling; no ISR needed
  gpio_config(&cfg);
}

void WakeButtonMgr::start()
{
  xTaskCreate(task_fn, "wake_btn", TASK_STACK, this, 5, nullptr);
}

ButtonEvent WakeButtonMgr::wait_for_event_timeout(uint32_t timeout_ms)
{
  ButtonEvent evt = ButtonEvent::EVT_NONE;
  xQueueReceive(event_queue_, &evt, pdMS_TO_TICKS(timeout_ms));
  return evt;
}

void WakeButtonMgr::push(ButtonEvent evt)
{
  xQueueSend(event_queue_, &evt, 0);
}

void WakeButtonMgr::task_fn(void * arg)
{
  static_cast<WakeButtonMgr *>(arg)->task_body();
}

void WakeButtonMgr::task_body()
{
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));

    int     level = gpio_get_level(pin_);
    int64_t now   = esp_timer_get_time() / 1000;  // µs → ms

    switch (state_) {

      case State::IDLE:
        if (level == 0) {
          state_     = State::DEBOUNCE_DOWN;
          t_edge_ms_ = now;
        }
        break;

      case State::DEBOUNCE_DOWN:
        if (level == 1) {
          // glitch — back to idle
          state_ = State::IDLE;
        } else if (now - t_edge_ms_ >= DEBOUNCE_MS) {
          state_ = State::DOWN;
          // keep t_edge_ms_ as press start time
        }
        break;

      case State::DOWN:
        if (now - t_edge_ms_ >= VLONG_MS) {
          push(ButtonEvent::EVT_VLONG);
          state_ = State::DRAIN;
        } else if (level == 1) {
          int64_t held = now - t_edge_ms_;
          if (held >= LONG_MS) {
            push(ButtonEvent::EVT_LONG);
            state_ = State::IDLE;
          } else {
            t_edge_ms_ = now;
            state_     = State::DEBOUNCE_UP;
          }
        }
        break;

      case State::DEBOUNCE_UP:
        if (level == 0) {
          // glitch — still pressed
          state_ = State::DOWN;
        } else if (now - t_edge_ms_ >= DEBOUNCE_MS) {
          t_edge_ms_ = now;   // start of double-tap wait window
          state_     = State::WAIT_SECOND;
        }
        break;

      case State::WAIT_SECOND:
        if (now - t_edge_ms_ >= DOUBLE_GAP_MS) {
          push(ButtonEvent::EVT_SHORT);
          state_ = State::IDLE;
        } else if (level == 0) {
          push(ButtonEvent::EVT_DOUBLE);
          state_ = State::DRAIN;
        }
        break;

      case State::DRAIN:
        // after LONG / VLONG / DOUBLE: ignore until fully released
        if (level == 1) {
          state_ = State::IDLE;
        }
        break;
    }
  }
}
