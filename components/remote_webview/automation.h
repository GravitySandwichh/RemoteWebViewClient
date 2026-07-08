#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "remote_webview.h"

namespace esphome {
namespace remote_webview {

// Automation things below
template<typename... Ts> class TriggerOnFrameUpdateAction : public Action<Ts...> {
 public:
  explicit TriggerOnFrameUpdateAction(RemoteWebView *ea) : ea_(ea) {}

  void play(Ts... x) override {
    this->ea_->trigger_on_frame_update();
  }

 protected:
  RemoteWebView *ea_;
};

// On Frame Update Trigger
class OnFrameUpdateTrigger : public Trigger<> {
 public:
  explicit OnFrameUpdateTrigger(RemoteWebView *parent) {
    parent->add_on_frame_update_callback([this]() { this->trigger(); });
  }
};

}  // namespace remote_webview
}  // namespace esphome
