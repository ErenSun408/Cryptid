#pragma once

#include <string>

namespace usb_vault::ui {

// Selects container files on behalf of the authenticated product page.
//
// The page never supplies or receives a path: the native side owns the whole
// selection, so no page-controlled string reaches the file system and no path
// crosses the automation boundary in either direction. Implementations must
// also leave no local trace of the selection, because a remembered folder or
// recent-file entry would record the container inventory that section 8.4
// forbids.
class IContainerFileChooser {
 public:
  virtual ~IContainerFileChooser() noexcept = default;

  IContainerFileChooser(const IContainerFileChooser&) = delete;
  IContainerFileChooser& operator=(const IContainerFileChooser&) = delete;
  IContainerFileChooser(IContainerFileChooser&&) = delete;
  IContainerFileChooser& operator=(IContainerFileChooser&&) = delete;

  // Returns an absolute path for a container file to create, or an empty
  // string when the user cancels. Cancellation is an ordinary outcome and
  // must not be reported as a failure.
  [[nodiscard]] virtual std::wstring choose_new_container() = 0;

  // Returns the absolute path of an existing container file to adopt, or an
  // empty string when the user cancels.
  [[nodiscard]] virtual std::wstring choose_existing_container() = 0;

 protected:
  IContainerFileChooser() noexcept = default;
};

}  // namespace usb_vault::ui
