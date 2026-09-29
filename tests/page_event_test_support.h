#pragma once

#include "usb_vault/diagnostics/debug_log.h"
#include <OleAuto.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace page_event_tests {
inline void check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
inline DISPID id(IDispatch& host) {
  auto* name = const_cast<wchar_t*>(L"reportPageEvent");
  DISPID result{};
  check(host.GetIDsOfNames(IID_NULL, &name, 1, LOCALE_INVARIANT, &result) == S_OK,
        "reportPageEvent is missing");
  return result;
}
struct Arguments final {
  std::array<VARIANTARG, 3> values{};
  DISPPARAMS parameters{values.data(), nullptr, 3, 0};
  Arguments(const std::wstring_view source = L"creation-status",
            const std::wstring_view content = L"创建失败（错误码2147500037）", LONG category = 1) {
    values[0].vt = values[1].vt = VT_BSTR;
    values[0].bstrVal = ::SysAllocStringLen(content.data(), static_cast<UINT>(content.size()));
    values[1].bstrVal = ::SysAllocStringLen(source.data(), static_cast<UINT>(source.size()));
    values[2].vt = VT_I4;
    values[2].lVal = category;
    check(values[0].bstrVal && values[1].bstrVal, "page event allocation failed");
  }
  ~Arguments() { for (auto& value : values) ::VariantClear(&value); }
  Arguments(const Arguments&) = delete;
  Arguments& operator=(const Arguments&) = delete;
};
inline HRESULT send(IDispatch& host, Arguments& args, UINT* argument_error = nullptr,
                    WORD flags = DISPATCH_METHOD, REFIID iid = IID_NULL) {
  VARIANT result{};
  ::SetLastError(ERROR_BUSY);
  const auto hr = host.Invoke(id(host), iid, LOCALE_INVARIANT, flags,
                              &args.parameters, &result, nullptr, argument_error);
  check(::GetLastError() == ERROR_BUSY, "page reporting changed the last Win32 error");
  if (hr == S_OK) check(result.vt == VT_BOOL && result.boolVal == VARIANT_TRUE,
                        "page reporting did not return true");
  ::VariantClear(&result);
  for (const auto& value : args.values) {
    if (value.vt == VT_BSTR && value.bstrVal)
      check(std::all_of(value.bstrVal, value.bstrVal + ::SysStringLen(value.bstrVal),
                        [](wchar_t ch) { return ch == 0; }), "page report text was not wiped");
  }
  return hr;
}
inline HRESULT send(IDispatch& host) { Arguments args; return send(host, args); }

struct Log final {
  std::filesystem::path root = std::filesystem::temp_directory_path() /
      (L"usb-vault-page-report-" + std::to_wstring(::GetCurrentProcessId()) + L"-" +
       std::to_wstring(::GetTickCount64()));
  std::filesystem::path path = root / L"logs" / L"runtime-log-20260927-120000.txt";
  usb_vault::diagnostics::DebugLogFile file;
  Log() {
    check(std::filesystem::create_directory(root), "page log fixture already exists");
    const SYSTEMTIME time{2026, 9, 0, 27, 12, 0, 0, 0};
    check(file.open(root, time), "cannot open page log fixture");
    usb_vault::diagnostics::set_runtime_log(&file);
  }
  ~Log() {
    usb_vault::diagnostics::set_runtime_log(nullptr);
    file.close();
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(root / L"logs", ignored);
    std::filesystem::remove(root, ignored);
  }
  std::string read() {
    std::ifstream input(path, std::ios::binary);
    check(input.good(), "cannot read page log fixture");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
};

// Run the same wire and log contract against both real IDispatch objects.
inline void verify_contract(IDispatch& host) {
  Log log;
  const auto initial = log.read();
  for (int scenario = 0; scenario < 13; ++scenario) {
    Arguments args(L"rejected-source", L"REJECTED_SECRET_SENTINEL");
    HRESULT expected = DISP_E_TYPEMISMATCH;
    UINT index = 99;
    if (scenario == 0) { args.parameters.cArgs = 2; expected = DISP_E_BADPARAMCOUNT; }
    if (scenario == 1) { args.parameters.cNamedArgs = 1; expected = DISP_E_NONAMEDARGS; }
    if (scenario == 2) { args.values[2].vt = VT_BOOL; args.values[2].boolVal = VARIANT_TRUE; }
    if (scenario >= 3 && scenario <= 6) {
      args.values[2].vt = VT_R8;
      args.values[2].dblVal = scenario == 3 ? 0 : scenario == 4 ? 5 : scenario == 5 ? 1.5 :
          (std::numeric_limits<double>::quiet_NaN)();
      expected = E_INVALIDARG;
    }
    if (scenario == 7 || scenario == 8) {
      const int target = scenario - 7;
      ::VariantClear(&args.values[target]);
      args.values[target].vt = VT_I4;
    }
    if (scenario == 9) { args.values[2].vt = VT_R8; args.values[2].dblVal =
        (std::numeric_limits<double>::infinity)(); expected = E_INVALIDARG; }
    if (scenario == 10) { args.values[2].lVal = -1; expected = E_INVALIDARG; }
    if (scenario == 11) expected = DISP_E_MEMBERNOTFOUND;
    if (scenario == 12) expected = DISP_E_UNKNOWNINTERFACE;
    check(send(host, args, &index, scenario == 11 ? DISPATCH_PROPERTYGET : DISPATCH_METHOD,
                scenario == 12 ? IID_IUnknown : IID_NULL) == expected, "invalid page event was not rejected");
    if (expected == DISP_E_TYPEMISMATCH)
      check(index == static_cast<UINT>(scenario == 7 ? 0 : scenario == 8 ? 1 : 2),
            "page event type error named the wrong reversed argument");
  }
  check(log.read() == initial, "rejected page event leaked through ordinary call tracing");
  {
    Arguments args;
    ::VariantClear(&args.values[0]);
    args.values[0].vt = VT_ARRAY | VT_UI1;
    args.values[0].parray = ::SafeArrayCreateVector(VT_UI1, 0, 4);
    check(args.values[0].parray != nullptr, "page array allocation failed");
    void* data{};
    check(SUCCEEDED(::SafeArrayAccessData(args.values[0].parray, &data)), "page array access failed");
    std::fill_n(static_cast<unsigned char*>(data), 4, static_cast<unsigned char>('S'));
    ::SafeArrayUnaccessData(args.values[0].parray);
    check(send(host, args) == DISP_E_TYPEMISMATCH, "page report accepted a credential array");
    check(SUCCEEDED(::SafeArrayAccessData(args.values[0].parray, &data)), "page array inspection failed");
    const auto* bytes = static_cast<const unsigned char*>(data);
    const bool cleared = std::all_of(bytes, bytes + 4, [](unsigned char ch) { return ch == 0; });
    ::SafeArrayUnaccessData(args.values[0].parray);
    check(cleared && log.read() == initial, "rejected page array survived or was logged");
  }
  for (LONG category = 1; category <= 4; ++category) {
    Arguments args(L"creation-status", L"创建失败（错误码2147500037）", category);
    if (category == 2) { args.values[2].vt = VT_UI4; args.values[2].ulVal = 2; }
    if (category == 3) { args.values[2].vt = VT_R8; args.values[2].dblVal = 3; }
    check(send(host, args) == S_OK, "valid page event was rejected");
  }
  {
    Arguments args(std::wstring(64, L'S') + L"SOURCE_TAIL", std::wstring(2000, L'C') + L"CONTENT_TAIL");
    check(send(host, args) == S_OK, "long page event was rejected instead of truncated");
  }
  {
    const wchar_t content[]{L'A', L'\0', L'\r', L'\n', L'\t', 0x1B, 0x85, 0x2028, 0x2029, 0x202E, L'Z'};
    Arguments args(L"source\nline", std::wstring_view(content, std::size(content)));
    check(send(host, args) == S_OK, "page control characters were rejected");
  }
  {
    Arguments args(std::wstring(63, L'U') + L"\xD83D\xDE00", L"emoji \xD83D\xDE00 lone \xDC00");
    check(send(host, args) == S_OK, "page surrogate text was rejected");
  }
#if USB_VAULT_DEBUG_LOG
  auto text = log.read();
  check(std::count(text.begin(), text.end(), '\n') == 8, "page event duplicated or injected a log line");
  check(text.find("[页面] 1 显示给用户的错误；来源 creation-status；内容 创建失败（错误码2147500037）\r\n") != std::string::npos,
        "page log lost category, source or the displayed error code");
  for (const auto* category : {"2 页面捕获的错误与校验失败", "3 未捕获的脚本异常", "4 页面放弃等待"})
    check(text.find(category) != std::string::npos, "page log lost a category");
  check(text.find(std::string(64, 'S') + "；内容 " + std::string(2000, 'C') + "\r\n") != std::string::npos &&
        text.find("SOURCE_TAIL") == std::string::npos && text.find("CONTENT_TAIL") == std::string::npos,
        "page log did not truncate at the required bounds");
  check(text.find("来源 source line；内容 A         Z\r\n") != std::string::npos,
        "page log did not neutralize embedded controls");
  check(text.find(std::string(63, 'U') + " ；内容 emoji \xF0\x9F\x98\x80 lone  \r\n") != std::string::npos,
        "page log damaged UTF-16 boundaries");
  check(text.find("页面调用") == std::string::npos, "page event also produced an ordinary call trace");
#else
  check(log.read() == initial, "disabled page logging wrote text");
#endif
  for (int i = 0; i < 200; ++i) check(send(host) == S_OK, "rate limiting changed page behavior");
  log.file.close();
#if USB_VAULT_DEBUG_LOG
  const auto limited = log.read();
  check(std::count(limited.begin(), limited.end(), '\n') == 202 &&
        limited.ends_with("[页面] 限流；已丢弃 7 条\r\n"), "page quota or close summary is incorrect");
#else
  check(log.read() == initial, "disabled page logging produced a quota summary");
#endif
  check(send(host) == S_OK, "a closed log changed page behavior");
}
}  // namespace page_event_tests
