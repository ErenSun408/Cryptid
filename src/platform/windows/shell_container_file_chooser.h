#pragma once

#include "usb_vault/ui/container_file_chooser.h"

#include <Windows.h>

#include <string>

namespace usb_vault::platform::windows {

// Shell file dialogs for picking container files, kept outside the
// vendor-neutral core like the other Windows adapters.
//
// Selection state is deliberately not persisted. The dialogs run without a
// client GUID and with `FOS_DONTADDTORECENT`, so neither a most-recently-used
// list, a remembered folder, nor a recent-documents shortcut records a
// container path. That is a requirement, not a preference: a remembered
// location is exactly the kind of local artifact section 8.4 forbids.
class ShellContainerFileChooser final : public ui::IContainerFileChooser {
 public:
  explicit ShellContainerFileChooser(HWND owner) noexcept;

  [[nodiscard]] std::wstring choose_container_directory() override;
  [[nodiscard]] std::wstring choose_existing_container() override;

 private:
  HWND owner_;
};

}  // namespace usb_vault::platform::windows
