#pragma once

#include "usb_vault/diagnostics/debug_log.h"
#include "usb_vault/ui/webview_password_array.h"

#include <OleAuto.h>
#include <algorithm>
#include <cmath>

namespace usb_vault::ui {

// Bounded UTF-16 input, including embedded NULs. Neutralize line separators,
// terminal controls and bidi controls; never leave half of a surrogate pair.
inline std::wstring clean_page_event_text(BSTR text, const std::size_t limit) {
  const std::size_t length = text == nullptr ? 0 : ::SysStringLen(text);
  const std::size_t size = (std::min)(length, limit);
  std::wstring result;
  result.reserve(size);
  for (std::size_t i = 0; i < size; ++i) {
    const wchar_t ch = text[i];
    if (ch >= 0xD800 && ch <= 0xDBFF) {
      if (i + 1 < size && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF) {
        result.push_back(ch);
        result.push_back(text[++i]);
      } else result.push_back(L' ');
    } else if (ch < 0x20 || (ch >= 0x7F && ch <= 0x9F) ||
               ch == 0x2028 || ch == 0x2029 || ch == 0x061C ||
               ch == 0x200E || ch == 0x200F || (ch >= 0x202A && ch <= 0x202E) ||
               (ch >= 0x2066 && ch <= 0x2069) || (ch >= 0xDC00 && ch <= 0xDFFF)) {
      result.push_back(L' ');
    } else result.push_back(ch);
  }
  return result;
}

// This entry point is intentionally independent of all session/controller
// state. Dispatch must reach it before argument tracing and business gates.
// Only safe diagnostic text belongs here, never credentials or card payloads.
inline HRESULT report_page_event(REFIID iid, WORD flags, DISPPARAMS* parameters,
                                  VARIANT* result, UINT* argument_error) noexcept {
  const DWORD last_error = ::GetLastError();
  struct Restore final {
    DWORD value;
    ~Restore() { ::SetLastError(value); }
  } restore{last_error};
  const DispatchTextWiper text_wiper(parameters);
  struct WipeArrays final {
    DISPPARAMS* args;
    ~WipeArrays() {
      if (args && args->rgvarg)
        for (UINT i = 0; i < args->cArgs; ++i) wipe_webview_password_array(args->rgvarg[i]);
    }
  } wipe_arrays{parameters};
  if (iid != IID_NULL) return DISP_E_UNKNOWNINTERFACE;
  if (flags != DISPATCH_METHOD) return DISP_E_MEMBERNOTFOUND;
  if (!parameters || parameters->cArgs != 3) return DISP_E_BADPARAMCOUNT;
  if (parameters->cNamedArgs) return DISP_E_NONAMEDARGS;
  if (!parameters->rgvarg) return E_POINTER;
  const auto mismatch = [&](UINT index) {
    if (argument_error) *argument_error = index;
    return DISP_E_TYPEMISMATCH;
  };
  auto& values = parameters->rgvarg;
  if (values[0].vt != VT_BSTR) return mismatch(0);
  if (values[1].vt != VT_BSTR) return mismatch(1);
  double category{};
  switch (values[2].vt) {
    case VT_I4: category = values[2].lVal; break;
    case VT_UI4: category = values[2].ulVal; break;
    case VT_R8: category = values[2].dblVal; break;
    default: return mismatch(2);
  }
  if (!std::isfinite(category) || category < 1 || category > 4 || std::floor(category) != category)
    return E_INVALIDARG;
#if USB_VAULT_DEBUG_LOG
  // No copies or formatting at all when debug logging is disabled or absent.
  if (auto* file = diagnostics::runtime_log_slot().load()) {
    try {
      constexpr const char* names[]{"", "1 显示给用户的错误", "2 页面捕获的错误与校验失败",
                                    "3 未捕获的脚本异常", "4 页面放弃等待"};
      SYSTEMTIME time{};
      ::GetLocalTime(&time);
      char prefix[96]{};
      std::snprintf(prefix, sizeof(prefix), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [线程 %lu] [页面] ",
                    time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                    time.wSecond, time.wMilliseconds, static_cast<unsigned long>(::GetCurrentThreadId()));
      const std::string line = std::string(prefix) + names[static_cast<unsigned int>(category)] +
          "；来源 " + diagnostics::utf8(clean_page_event_text(values[1].bstrVal, 64)) +
          "；内容 " + diagnostics::utf8(clean_page_event_text(values[0].bstrVal, 2000)) + "\r\n";
      file->write_page_event(line);
    } catch (...) {
      // Do not log reporting failures, even to the ordinary exception logger.
    }
  }
#endif
  if (result) {
    ::VariantInit(result);
    result->vt = VT_BOOL;
    result->boolVal = VARIANT_TRUE;
  }
  return S_OK;
}

}  // namespace usb_vault::ui
