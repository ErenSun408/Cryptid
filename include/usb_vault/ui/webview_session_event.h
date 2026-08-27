#pragma once

namespace usb_vault::ui {

// Fixed, data-free notification used by the native window after an external
// session-state transition. The page must query the existing unified mounted
// state instead of receiving device, mode, or volume details in the event.
inline constexpr wchar_t kWebviewSessionChangedScript[] =
    L"window.dispatchEvent(new Event('usb-vault-session-changed'));";

}  // namespace usb_vault::ui
