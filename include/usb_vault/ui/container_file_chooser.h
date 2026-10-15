#pragma once

#include <string>

namespace usb_vault::ui {

// Locates container files on behalf of the authenticated product page.
//
// Creation and adoption no longer share one rule, because the page no longer
// plays the same part in both.
//
// Adoption is unchanged: the native side owns the whole selection and the
// page never learns the path of the container it opened.
//
// Creation is deliberately different since 2026-09-05. The page owns the name
// and the save location, and only browses here when the user asks for a picker
// instead of typing. A directory therefore crosses the boundary in both
// directions, and a page-supplied string does reach the file system — under
// `resolve_container_target_path`, which restates as an explicit check every
// guarantee the save dialog used to give structurally. Nothing weaker than
// that function may stand between the page and a created file.
//
// Implementations must still leave no local trace of any selection, because a
// remembered folder or recent-file entry would record the container inventory
// that section 8.4 forbids. That obligation did not move with the path.
class IContainerFileChooser {
 public:
  virtual ~IContainerFileChooser() noexcept = default;

  IContainerFileChooser(const IContainerFileChooser&) = delete;
  IContainerFileChooser& operator=(const IContainerFileChooser&) = delete;
  IContainerFileChooser(IContainerFileChooser&&) = delete;
  IContainerFileChooser& operator=(IContainerFileChooser&&) = delete;

  // Returns an absolute directory to save a new container into, or an empty
  // string when the user cancels. Cancellation is an ordinary outcome and must
  // not be reported as a failure. This only fills the form's save-location
  // field; it decides nothing on its own, and the value it returns is checked
  // again on the way back in exactly like a typed one.
  [[nodiscard]] virtual std::wstring choose_container_directory() = 0;

  // Returns the absolute path of an existing container file to adopt, or an
  // empty string when the user cancels. The path stays on this side.
  [[nodiscard]] virtual std::wstring choose_existing_container() = 0;

  // P5.9: returns the absolute path of an encrypted export package to import,
  // or an empty string when the user cancels. Like adoption, the path stays
  // on this side; the page learns only the file name.
  [[nodiscard]] virtual std::wstring choose_import_package() { return {}; }

 protected:
  IContainerFileChooser() noexcept = default;
};

}  // namespace usb_vault::ui
