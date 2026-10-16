#pragma once

#include <cstddef>
#include <mutex>

namespace usb_vault::ui {

// Explorer's "show hidden items" and "show protected operating system files"
// switches. The shell file dialog honours `FOS_FORCESHOWHIDDEN` for plain
// hidden items only; Hidden+System items, which every file secure drive now
// carries, stay invisible unless the second switch is on.
struct HiddenItemsVisibility final {
  bool hidden_items{};
  bool protected_system_items{};

  friend bool operator==(const HiddenItemsVisibility&,
                         const HiddenItemsVisibility&) = default;
};

class IHiddenItemsSettings {
 public:
  virtual ~IHiddenItemsSettings() = default;
  [[nodiscard]] virtual HiddenItemsVisibility read() noexcept = 0;
  virtual void write(HiddenItemsVisibility visibility) noexcept = 0;
};

// Turns both switches on while at least one scope is alive and restores the
// captured values when the last scope ends. The switches are global for the
// signed-in account, so a scope must only span a modal file dialog. Nothing is
// written when both were already on, and a value changed elsewhere during the
// scope is left as found rather than overwritten.
class HiddenItemsRevealer final {
 public:
  class Scope final {
   public:
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;
    ~Scope() noexcept { owner_.release(); }

   private:
    friend class HiddenItemsRevealer;
    explicit Scope(HiddenItemsRevealer& owner) noexcept : owner_(owner) {}
    HiddenItemsRevealer& owner_;
  };

  explicit HiddenItemsRevealer(IHiddenItemsSettings& settings) noexcept
      : settings_(settings) {}
  HiddenItemsRevealer(const HiddenItemsRevealer&) = delete;
  HiddenItemsRevealer& operator=(const HiddenItemsRevealer&) = delete;

  [[nodiscard]] Scope reveal() noexcept;

 private:
  void release() noexcept;

  IHiddenItemsSettings& settings_;
  std::mutex mutex_;
  std::size_t active_{};
  bool restore_{};
  HiddenItemsVisibility original_{};
};

// The process-wide revealer over the shell's own settings store.
[[nodiscard]] HiddenItemsRevealer& shell_hidden_items_revealer() noexcept;

}  // namespace usb_vault::ui
