#pragma once

namespace usb_vault::ui {

// Fixed, data-free notification used by the native window after an external
// session-state transition. The page must query the existing unified mounted
// state instead of receiving device, mode, or volume details in the event.
inline constexpr wchar_t kWebviewSessionChangedScript[] =
    L"window.dispatchEvent(new Event('usb-vault-session-changed'));";

// Mount inventory changes do not revoke authentication or reset the page.
inline constexpr wchar_t kWebviewMountsChangedScript[] =
    L"window.dispatchEvent(new Event('usb-vault-mounts-changed'));";

// Raised when a shell drop the window accepted starts or stops running. Like
// the event above it carries nothing: the page reads the run's four numbers
// back through the bridge, and never learns what was dropped.
inline constexpr wchar_t kWebviewExternalDropChangedScript[] =
    L"window.dispatchEvent(new Event('usb-vault-external-drop-changed'));";

// Native activation is authoritative even when the embedded document keeps
// focus while another application is foreground. No session data is carried.
inline constexpr wchar_t kWebviewActivatedScript[] =
    L"window.dispatchEvent(new Event('usb-vault-window-activated'));";

}  // namespace usb_vault::ui
