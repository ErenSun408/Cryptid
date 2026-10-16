#include "usb_vault/ui/hidden_items_reveal.h"

#include <iostream>
#include <stdexcept>

namespace {

using usb_vault::ui::HiddenItemsRevealer;
using usb_vault::ui::HiddenItemsVisibility;
using usb_vault::ui::IHiddenItemsSettings;

constexpr HiddenItemsVisibility kHidden{false, false};
constexpr HiddenItemsVisibility kRevealed{true, true};

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

class FakeSettings final : public IHiddenItemsSettings {
 public:
  explicit FakeSettings(const HiddenItemsVisibility value) : value_(value) {}

  [[nodiscard]] HiddenItemsVisibility read() noexcept override { return value_; }
  void write(const HiddenItemsVisibility value) noexcept override {
    value_ = value;
    ++writes_;
  }

  HiddenItemsVisibility value_;
  int writes_{};
};

void test_reveals_and_restores_original_values() {
  FakeSettings settings({true, false});
  HiddenItemsRevealer revealer(settings);
  {
    const auto scope = revealer.reveal();
    require(settings.value_ == kRevealed, "both switches are on while shown");
  }
  require(settings.value_ == HiddenItemsVisibility{true, false},
          "the captured values are restored");
  require(settings.writes_ == 2, "one reveal and one restore are written");
}

void test_already_revealed_settings_are_never_written() {
  FakeSettings settings(kRevealed);
  HiddenItemsRevealer revealer(settings);
  { const auto scope = revealer.reveal(); }
  require(settings.value_ == kRevealed, "settings stay revealed");
  require(settings.writes_ == 0, "nothing is written when already on");
}

void test_nested_scopes_restore_once_after_the_last() {
  FakeSettings settings(kHidden);
  HiddenItemsRevealer revealer(settings);
  {
    const auto outer = revealer.reveal();
    {
      const auto inner = revealer.reveal();
      require(settings.writes_ == 1, "a nested scope does not write again");
    }
    require(settings.value_ == kRevealed, "the outer scope keeps them on");
  }
  require(settings.value_ == kHidden, "the last scope restores");
  require(settings.writes_ == 2, "exactly one restore");

  { const auto again = revealer.reveal(); }
  require(settings.value_ == kHidden && settings.writes_ == 4,
          "a later scope captures and restores afresh");
}

void test_a_change_made_elsewhere_is_kept() {
  FakeSettings settings(kHidden);
  HiddenItemsRevealer revealer(settings);
  {
    const auto scope = revealer.reveal();
    settings.value_ = {true, false};
  }
  require(settings.value_ == HiddenItemsVisibility{true, false},
          "a value changed during the scope is not overwritten");
  require(settings.writes_ == 1, "no restore over an outside change");
}

}  // namespace

int main() {
  try {
    test_reveals_and_restores_original_values();
    test_already_revealed_settings_are_never_written();
    test_nested_scopes_restore_once_after_the_last();
    test_a_change_made_elsewhere_is_kept();
  } catch (const std::exception& error) {
    std::cerr << "hidden_items_reveal_tests: FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "hidden_items_reveal_tests: PASS\n";
  return 0;
}
