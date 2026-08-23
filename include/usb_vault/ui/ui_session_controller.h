#pragma once

#include "usb_vault/application/application_session.h"

#include <cstddef>
#include <span>

namespace usb_vault::ui {

// Thin UI boundary: consumes one mutable password bridge allocation and
// exposes only a unified mounted state. It does not classify volumes or retain
// password bytes.
class UiSessionController final {
 public:
  explicit UiSessionController(
      application::IApplicationSessionActions& session) noexcept;

  void submit_password(std::span<std::byte> mutable_utf8);
  void unmount();
  [[nodiscard]] bool mounted() const noexcept;

 private:
  application::IApplicationSessionActions& session_;
};

}  // namespace usb_vault::ui
