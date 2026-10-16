// Activation code issuer. Runs only on the issuing machine: it owns the ECDSA
// signing key, which never ships with the application. The working copy of
// the key is DPAPI-protected for the current Windows account; a password
// backup is the only form that may leave the machine.

#include "usb_vault/licensing/activation_code_v1.h"
#include "usb_vault/licensing/signing_key_backup_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <Windows.h>
#include <CommCtrl.h>
#include <ShlObj.h>
#include <commdlg.h>
#include <dpapi.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using usb_vault::licensing::ActivationGrant;
using usb_vault::licensing::ActivationPublicKey;
using usb_vault::licensing::ActivationSigner;
using usb_vault::security::SecureBuffer;

constexpr wchar_t kWindowClass[] = L"UsbVaultActivationIssuer";
constexpr wchar_t kTitle[] = L"USB Vault 激活码签发";
constexpr char kEntropy[] = "usb-vault activation signing key v1";
constexpr std::size_t kMinimumBackupPasswordCodePoints = 12;

enum Control : int {
  kCardLabel = 100, kCard, kZoneLabel, kZone, kDateLabel, kDate, kTimeLabel,
  kTime, kUtc, kGenerate, kCodeLabel, kCode, kCopy, kStatus, kKeyLabel,
  kPublicKey, kPasswordLabel, kPassword, kBackup, kRestore, kRecords,
  kCreateKey,
};

// Offsets in use somewhere in the world, east of UTC positive.
constexpr int kZoneMinutes[] = {
    -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180,
    -120, -60,  0,    60,   120,  180,  210,  240,  270,  300,  330,  345,
    360,  390,  420,  480,  525,  540,  570,  600,  630,  660,  720,  765,
    780,  840};

HWND g_window{};
HFONT g_font{};
std::optional<ActivationSigner> g_signer;

// ---------------------------------------------------------------- text ---

[[nodiscard]] std::wstring widen(const std::string_view text) {
  if (text.empty()) return {};
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size);
  return result;
}

[[nodiscard]] std::string narrow(const std::wstring_view text) {
  if (text.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string result(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), size, nullptr, nullptr);
  return result;
}

[[nodiscard]] std::wstring window_text(const int id) {
  const HWND control = ::GetDlgItem(g_window, id);
  const int length = ::GetWindowTextLengthW(control);
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  ::GetWindowTextW(control, text.data(), length + 1);
  text.resize(static_cast<std::size_t>(length));
  return text;
}

[[nodiscard]] std::wstring zone_label(const int minutes) {
  const int magnitude = minutes < 0 ? -minutes : minutes;
  wchar_t label[16]{};
  std::swprintf(label, std::size(label), L"UTC%c%02d:%02d",
                minutes < 0 ? L'-' : L'+', magnitude / 60, magnitude % 60);
  return label;
}

[[nodiscard]] std::wstring format_time(const SYSTEMTIME& time) {
  wchar_t text[32]{};
  std::swprintf(text, std::size(text), L"%04u-%02u-%02u %02u:%02u:%02u",
                time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                time.wSecond);
  return text;
}

[[nodiscard]] SYSTEMTIME utc_time(const std::int64_t seconds) {
  ULARGE_INTEGER ticks;
  ticks.QuadPart = static_cast<ULONGLONG>(seconds + 11644473600LL) * 10000000ULL;
  FILETIME file{ticks.LowPart, ticks.HighPart};
  SYSTEMTIME time{};
  ::FileTimeToSystemTime(&file, &time);
  return time;
}

[[nodiscard]] std::int64_t now_utc_seconds() {
  FILETIME file{};
  ::GetSystemTimeAsFileTime(&file);
  ULARGE_INTEGER ticks{{file.dwLowDateTime, file.dwHighDateTime}};
  return static_cast<std::int64_t>(ticks.QuadPart / 10000000ULL) - 11644473600LL;
}

void show_error(const std::wstring& message) {
  ::MessageBoxW(g_window, message.c_str(), kTitle, MB_OK | MB_ICONERROR);
}

void set_status(const std::wstring& text) {
  ::SetWindowTextW(::GetDlgItem(g_window, kStatus), text.c_str());
}

// Reads a password field into page-locked UTF-8 and clears the field.
[[nodiscard]] SecureBuffer take_password(std::size_t& code_points) {
  const HWND control = ::GetDlgItem(g_window, kPassword);
  const int length = ::GetWindowTextLengthW(control);
  std::vector<wchar_t> wide(static_cast<std::size_t>(length) + 1, L'\0');
  ::GetWindowTextW(control, wide.data(), length + 1);
  // The edit control frees its buffer without wiping it; writing a filler of
  // the same length first overwrites the password in place.
  ::SetWindowTextW(control, std::wstring(static_cast<std::size_t>(length), L'*').c_str());
  ::SetWindowTextW(control, L"");
  code_points = 0;
  for (int i = 0; i < length; ++i) {
    if (!IS_LOW_SURROGATE(wide[static_cast<std::size_t>(i)])) ++code_points;
  }
  const int bytes = length == 0 ? 0
      : ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length,
                              nullptr, 0, nullptr, nullptr);
  SecureBuffer result(static_cast<std::size_t>(bytes));
  if (bytes > 0) {
    ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length,
                          reinterpret_cast<char*>(result.data()), bytes, nullptr,
                          nullptr);
  }
  ::SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
  return result;
}

// ---------------------------------------------------------------- files ---

[[nodiscard]] std::wstring data_directory() {
  PWSTR base = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                    nullptr, &base))) {
    throw std::runtime_error("LocalAppData is unavailable");
  }
  std::wstring directory = std::wstring(base) + L"\\usb-vault-issuer";
  ::CoTaskMemFree(base);
  if (!::CreateDirectoryW(directory.c_str(), nullptr) &&
      ::GetLastError() != ERROR_ALREADY_EXISTS) {
    throw std::runtime_error("cannot create the issuer directory");
  }
  return directory;
}

[[nodiscard]] std::wstring key_path() { return data_directory() + L"\\signing-key.dat"; }
[[nodiscard]] std::wstring records_path() { return data_directory() + L"\\issued.csv"; }

[[nodiscard]] std::optional<std::vector<std::byte>> read_file(
    const std::wstring& path, const std::size_t limit) {
  const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (::GetLastError() == ERROR_FILE_NOT_FOUND) return std::nullopt;
    throw std::runtime_error("cannot open a file");
  }
  LARGE_INTEGER size{};
  std::vector<std::byte> bytes;
  bool ok = ::GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0 &&
            static_cast<ULONGLONG>(size.QuadPart) <= limit;
  if (ok) {
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read,
                    nullptr) != FALSE && read == bytes.size();
  }
  ::CloseHandle(file);
  if (!ok) throw std::runtime_error("cannot read a file");
  return bytes;
}

// Writes beside the target and renames over it, so a failed write never
// leaves a truncated key or backup in place.
void replace_file(const std::wstring& path, const std::span<const std::byte> bytes) {
  const std::wstring temporary = path + L".tmp";
  const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create a file");
  DWORD written = 0;
  const bool ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr) != FALSE &&
                  written == bytes.size() && ::FlushFileBuffers(file) != FALSE;
  ::CloseHandle(file);
  if (!ok || !::MoveFileExW(temporary.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(temporary.c_str());
    throw std::runtime_error("cannot write a file");
  }
}

[[nodiscard]] DATA_BLOB entropy() {
  return {static_cast<DWORD>(sizeof(kEntropy) - 1),
          reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
}

void save_signer(const ActivationSigner& signer) {
  DATA_BLOB input{static_cast<DWORD>(signer.private_blob().size()),
                  reinterpret_cast<BYTE*>(
                      const_cast<std::byte*>(signer.private_blob().data()))};
  DATA_BLOB salt = entropy();
  DATA_BLOB output{};
  if (!::CryptProtectData(&input, L"usb-vault activation signing key", &salt,
                          nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    throw std::runtime_error("DPAPI could not protect the signing key");
  }
  const std::vector<std::byte> sealed(
      reinterpret_cast<std::byte*>(output.pbData),
      reinterpret_cast<std::byte*>(output.pbData) + output.cbData);
  ::LocalFree(output.pbData);
  replace_file(key_path(), sealed);
}

[[nodiscard]] std::optional<ActivationSigner> load_signer() {
  const auto sealed = read_file(key_path(), 4096);
  if (!sealed) return std::nullopt;
  DATA_BLOB input{static_cast<DWORD>(sealed->size()),
                  reinterpret_cast<BYTE*>(const_cast<std::byte*>(sealed->data()))};
  DATA_BLOB salt = entropy();
  DATA_BLOB output{};
  if (!::CryptUnprotectData(&input, nullptr, &salt, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    throw std::runtime_error(
        "DPAPI could not open the signing key; it belongs to another Windows account or machine");
  }
  SecureBuffer blob(output.cbData);
  std::copy_n(reinterpret_cast<std::byte*>(output.pbData), output.cbData,
              blob.bytes().begin());
  ::SecureZeroMemory(output.pbData, output.cbData);
  ::LocalFree(output.pbData);
  return ActivationSigner::import_private_blob(blob.bytes());
}

void append_record(const std::wstring& line) {
  const std::wstring path = records_path();
  const HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                    nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
  if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open the record");
  std::string text;
  if (::GetLastError() != ERROR_ALREADY_EXISTS) {
    text = "\xEF\xBB\xBF" + narrow(L"签发时间(UTC),卡编号,时区,到期(所选时区),到期(UTC),激活码\r\n");
  }
  text += narrow(line) + "\r\n";
  DWORD written = 0;
  const bool ok = ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
                              &written, nullptr) != FALSE && written == text.size();
  ::CloseHandle(file);
  if (!ok) throw std::runtime_error("cannot write the record");
}

// ---------------------------------------------------------------- logic ---

[[nodiscard]] std::wstring hex_key(const ActivationPublicKey& key) {
  std::wstring text;
  for (const std::byte value : key) {
    wchar_t pair[3]{};
    std::swprintf(pair, 3, L"%02X", std::to_integer<unsigned>(value));
    text += pair;
  }
  return text;
}

// Without a key nothing can be signed or backed up; the window offers to
// import one or, deliberately, to create one.
void show_public_key() {
  ::SetWindowTextW(::GetDlgItem(g_window, kPublicKey),
                   g_signer ? hex_key(g_signer->public_key()).c_str() : L"");
  const bool loaded = g_signer.has_value();
  ::EnableWindow(::GetDlgItem(g_window, kGenerate), loaded);
  ::EnableWindow(::GetDlgItem(g_window, kBackup), loaded);
  ::ShowWindow(::GetDlgItem(g_window, kCreateKey), loaded ? SW_HIDE : SW_SHOW);
}

// Accepts the card number as the application shows it: eight hexadecimal
// digits, optionally split by a hyphen or spaces.
[[nodiscard]] std::optional<std::uint32_t> parse_card(const std::wstring& text) {
  std::uint32_t value = 0;
  int digits = 0;
  for (const wchar_t c : text) {
    if (c == L'-' || c == L' ') continue;
    int digit = -1;
    if (c >= L'0' && c <= L'9') digit = c - L'0';
    if (c >= L'a' && c <= L'f') digit = c - L'a' + 10;
    if (c >= L'A' && c <= L'F') digit = c - L'A' + 10;
    if (digit < 0 || ++digits > 8) return std::nullopt;
    value = (value << 4) | static_cast<std::uint32_t>(digit);
  }
  if (digits != 8) return std::nullopt;
  return value;
}

[[nodiscard]] int selected_zone() {
  const LRESULT index = ::SendDlgItemMessageW(g_window, kZone, CB_GETCURSEL, 0, 0);
  if (index < 0 || static_cast<std::size_t>(index) >= std::size(kZoneMinutes)) return 0;
  return kZoneMinutes[index];
}

struct Expiry final {
  SYSTEMTIME local;
  std::int64_t utc_seconds;
};

[[nodiscard]] std::optional<Expiry> selected_expiry() {
  SYSTEMTIME date{};
  SYSTEMTIME time{};
  if (DateTime_GetSystemtime(::GetDlgItem(g_window, kDate), &date) != GDT_VALID ||
      DateTime_GetSystemtime(::GetDlgItem(g_window, kTime), &time) != GDT_VALID) {
    return std::nullopt;
  }
  Expiry expiry{};
  expiry.local = date;
  expiry.local.wHour = time.wHour;
  expiry.local.wMinute = time.wMinute;
  expiry.local.wSecond = time.wSecond;
  expiry.local.wMilliseconds = 0;
  const auto seconds = usb_vault::licensing::utc_seconds_from_offset_time(
      date.wYear, date.wMonth, date.wDay, time.wHour, time.wMinute, time.wSecond,
      selected_zone());
  if (!seconds) return std::nullopt;
  expiry.utc_seconds = *seconds;
  return expiry;
}

void refresh_utc() {
  const auto expiry = selected_expiry();
  const std::wstring text = expiry
      ? L"对应 UTC 时间：" + format_time(utc_time(expiry->utc_seconds))
      : L"日期或时间无效";
  ::SetWindowTextW(::GetDlgItem(g_window, kUtc), text.c_str());
}

void generate() {
  if (!g_signer) return show_error(L"签名私钥未加载。");
  const auto card = parse_card(window_text(kCard));
  if (!card) return show_error(L"卡编号应为 8 位十六进制数字，例如 1A2B-3C4D。");
  const auto expiry = selected_expiry();
  if (!expiry) return show_error(L"到期日期或时间无效。");
  if (expiry->utc_seconds <= now_utc_seconds()) {
    return show_error(L"到期时间已经过去，请选择将来的时间。");
  }
  const std::string code = g_signer->sign({*card, expiry->utc_seconds});
  const std::wstring wide_code = widen(code);
  ::SetWindowTextW(::GetDlgItem(g_window, kCode), L"");

  wchar_t card_text[16]{};
  std::swprintf(card_text, std::size(card_text), L"%04X-%04X", *card >> 16,
                *card & 0xFFFF);
  append_record(format_time(utc_time(now_utc_seconds())) + L"," + card_text +
                L"," + zone_label(selected_zone()) + L"," +
                format_time(expiry->local) + L"," +
                format_time(utc_time(expiry->utc_seconds)) + L"," + wide_code);
  // Shown only once recorded, so no code leaves the tool without an entry.
  ::SetWindowTextW(::GetDlgItem(g_window, kCode), wide_code.c_str());
  set_status(std::wstring(L"已为卡 ") + card_text + L" 生成激活码，并写入签发记录。");
}

void copy_code() {
  const std::wstring code = window_text(kCode);
  if (code.empty()) return;
  if (!::OpenClipboard(g_window)) return show_error(L"无法打开剪贴板。");
  ::EmptyClipboard();
  const std::size_t bytes = (code.size() + 1) * sizeof(wchar_t);
  if (const HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (void* const target = ::GlobalLock(memory)) {
      std::memcpy(target, code.c_str(), bytes);
      ::GlobalUnlock(memory);
      if (!::SetClipboardData(CF_UNICODETEXT, memory)) ::GlobalFree(memory);
    } else {
      ::GlobalFree(memory);
    }
  }
  // Keeps the code out of clipboard history and cloud clipboard sync.
  const UINT exclude =
      ::RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
  if (exclude != 0) {
    if (const HGLOBAL marker = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
      if (!::SetClipboardData(exclude, marker)) ::GlobalFree(marker);
    }
  }
  ::CloseClipboard();
  set_status(L"激活码已复制。");
}

[[nodiscard]] std::optional<std::wstring> choose_file(const bool save) {
  wchar_t path[MAX_PATH] = L"usb-vault-signing-key.uvkb";
  if (!save) path[0] = L'\0';
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = g_window;
  dialog.lpstrFilter = L"签名私钥备份 (*.uvkb)\0*.uvkb\0";
  dialog.lpstrFile = path;
  dialog.nMaxFile = MAX_PATH;
  dialog.lpstrDefExt = L"uvkb";
  dialog.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  const BOOL chosen = save ? ::GetSaveFileNameW(&dialog) : ::GetOpenFileNameW(&dialog);
  if (!chosen) return std::nullopt;
  return std::wstring(path);
}

void backup() {
  if (!g_signer) return show_error(L"签名私钥未加载。");
  std::size_t code_points = 0;
  const SecureBuffer password = take_password(code_points);
  if (code_points < kMinimumBackupPasswordCodePoints) {
    return show_error(L"备份口令至少 12 个字符。");
  }
  const auto path = choose_file(true);
  if (!path) return;
  ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
  replace_file(*path, seal_signing_key_backup(*g_signer, password));
  set_status(L"私钥备份已保存。请将备份文件与口令分开妥善保管。");
}

void restore() {
  std::size_t code_points = 0;
  const SecureBuffer password = take_password(code_points);
  if (code_points == 0) return show_error(L"请先在“备份口令”中输入该备份的口令。");
  const auto path = choose_file(false);
  if (!path) return;
  const auto bytes = read_file(*path, 4096);
  if (!bytes) return show_error(L"备份文件不存在。");
  ::SetCursor(::LoadCursorW(nullptr, IDC_WAIT));
  auto signer = usb_vault::licensing::open_signing_key_backup(*bytes, password);
  if (!signer) return show_error(L"口令错误或备份文件已损坏。");
  if (g_signer && signer->public_key() == g_signer->public_key()) {
    set_status(L"备份中的私钥与当前私钥相同，未作更改。");
    return;
  }
  if (!g_signer &&
      ::GetFileAttributesW(key_path().c_str()) != INVALID_FILE_ATTRIBUTES &&
      ::MessageBoxW(g_window,
                    L"现有私钥文件无法打开。导入将覆盖该文件，覆盖前请确认它已无用或已另行保存。确定覆盖？",
                    kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }
  if (g_signer &&
      ::MessageBoxW(g_window,
                    L"导入后将替换当前私钥。已分发的应用只认其内置公钥对应的私钥，"
                    L"替换后用当前私钥签发的激活码将无法再签发。确定替换？",
                    kTitle, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }
  save_signer(*signer);
  g_signer = std::move(signer);
  show_public_key();
  set_status(L"已从备份导入私钥。请核对公钥与交付说明中的一致。");
}

// A new key is never made silently: applications already distributed accept
// only codes from the key whose public half they embed.
void create_key() {
  if (g_signer) return;
  const bool existing =
      ::GetFileAttributesW(key_path().c_str()) != INVALID_FILE_ATTRIBUTES;
  const std::wstring warning = std::wstring(
      L"将生成新的签名私钥。已分发的应用只接受其内置公钥对应私钥签发的激活码；"
      L"如需沿用已有私钥，请取消并改用“从备份导入”。") +
      (existing ? L"\n\n现有私钥文件无法打开，生成后将被覆盖。" : L"") + L"\n\n确定生成？";
  if (::MessageBoxW(g_window, warning.c_str(), kTitle,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }
  ActivationSigner signer = ActivationSigner::generate();
  save_signer(signer);
  g_signer = std::move(signer);
  show_public_key();
  set_status(L"已生成新的签名私钥。请立即用“备份私钥”保存备份，并将公钥提供给应用构建。");
}

void open_records() {
  const std::wstring path = records_path();
  if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return show_error(L"尚无签发记录。");
  }
  wchar_t system[MAX_PATH]{};
  const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return show_error(L"找不到系统目录。");
  const std::wstring notepad = std::wstring(system) + L"\\notepad.exe";
  ::ShellExecuteW(g_window, L"open", notepad.c_str(), (L"\"" + path + L"\"").c_str(),
                  nullptr, SW_SHOWNORMAL);
}

void run_guarded(void (*action)()) {
  try {
    action();
  } catch (const std::exception& error) {
    show_error(L"操作失败：" + widen(error.what()));
  }
}

// --------------------------------------------------------------- layout ---

[[nodiscard]] int current_offset_minutes() {
  SYSTEMTIME utc{};
  SYSTEMTIME local{};
  ::GetSystemTime(&utc);
  if (!::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return 480;
  FILETIME a{};
  FILETIME b{};
  ::SystemTimeToFileTime(&utc, &a);
  ::SystemTimeToFileTime(&local, &b);
  const auto ticks = [](const FILETIME& f) {
    return static_cast<long long>((static_cast<ULONGLONG>(f.dwHighDateTime) << 32) |
                                  f.dwLowDateTime);
  };
  const long long minutes = (ticks(b) - ticks(a)) / 600000000LL;
  return static_cast<int>((minutes + (minutes >= 0 ? 7 : -7)) / 15 * 15);
}

HWND add(const wchar_t* const kind, const wchar_t* const text, const DWORD style,
         const int id, const DWORD extended = 0) {
  const HWND control = ::CreateWindowExW(
      extended, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
  ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
  return control;
}

void apply_font(const UINT dpi) {
  NONCLIENTMETRICSW metrics{};
  metrics.cbSize = sizeof(metrics);
  ::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics,
                               0, dpi);
  const HFONT font = ::CreateFontIndirectW(&metrics.lfMessageFont);
  for (HWND child = ::GetWindow(g_window, GW_CHILD); child != nullptr;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
  if (g_font) ::DeleteObject(g_font);
  g_font = font;
}

void layout() {
  const UINT dpi = ::GetDpiForWindow(g_window);
  const auto s = [dpi](const int value) { return ::MulDiv(value, static_cast<int>(dpi), 96); };
  RECT client{};
  ::GetClientRect(g_window, &client);
  const int margin = s(16);
  const int label = s(84);
  const int row = s(26);
  const int gap = s(10);
  const int width = client.right - 2 * margin;
  const int field = width - label;
  int y = margin;
  const auto place = [](const int id, const int x, const int top, const int w, const int h) {
    ::MoveWindow(::GetDlgItem(g_window, id), x, top, w, h, TRUE);
  };
  const auto labelled = [&](const int label_id, const int field_id, const int w, const int h) {
    place(label_id, margin, y + s(4), label, row);
    place(field_id, margin + label, y, w, h);
  };
  labelled(kCardLabel, kCard, s(160), row);
  y += row + gap;
  labelled(kZoneLabel, kZone, s(160), s(320));
  y += row + gap;
  labelled(kDateLabel, kDate, s(160), row);
  place(kTimeLabel, margin + label + s(176), y + s(4), s(70), row);
  place(kTime, margin + label + s(246), y, s(110), row);
  y += row + gap;
  place(kUtc, margin + label, y, field, row);
  y += row + gap;
  place(kGenerate, margin + label, y, s(120), s(30));
  y += s(30) + gap;
  labelled(kCodeLabel, kCode, field, s(72));
  y += s(72) + gap;
  place(kCopy, margin + label, y, s(120), s(30));
  y += s(30) + gap;
  place(kStatus, margin, y, width, row);
  y += row + s(18);
  labelled(kKeyLabel, kPublicKey, field, s(48));
  y += s(48) + gap;
  labelled(kPasswordLabel, kPassword, s(200), row);
  y += row + gap;
  place(kBackup, margin + label, y, s(110), s(30));
  place(kRestore, margin + label + s(120), y, s(110), s(30));
  place(kRecords, margin + label + s(240), y, s(110), s(30));
  place(kCreateKey, margin + label + s(360), y, s(110), s(30));
}

void create_controls() {
  add(WC_STATICW, L"卡编号", 0, kCardLabel);
  const HWND card = add(WC_EDITW, L"", WS_BORDER | WS_TABSTOP | ES_UPPERCASE, kCard);
  ::SendMessageW(card, EM_LIMITTEXT, 12, 0);

  add(WC_STATICW, L"时区", 0, kZoneLabel);
  const HWND zone = add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kZone);
  const int current = current_offset_minutes();
  int selected = 0;
  for (std::size_t i = 0; i < std::size(kZoneMinutes); ++i) {
    ::SendMessageW(zone, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(zone_label(kZoneMinutes[i]).c_str()));
    if (kZoneMinutes[i] == current || (kZoneMinutes[i] == 480 && selected == 0)) {
      selected = static_cast<int>(i);
    }
  }
  if (std::ranges::find(kZoneMinutes, current) != std::end(kZoneMinutes)) {
    selected = static_cast<int>(std::ranges::find(kZoneMinutes, current) - std::begin(kZoneMinutes));
  }
  ::SendMessageW(zone, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);

  add(WC_STATICW, L"到期日期", 0, kDateLabel);
  const HWND date = add(DATETIMEPICK_CLASSW, L"", DTS_SHORTDATEFORMAT | WS_TABSTOP, kDate);
  DateTime_SetFormat(date, L"yyyy'-'MM'-'dd");
  add(WC_STATICW, L"到期时间", 0, kTimeLabel);
  const HWND time = add(DATETIMEPICK_CLASSW, L"", DTS_TIMEFORMAT | DTS_UPDOWN | WS_TABSTOP, kTime);
  DateTime_SetFormat(time, L"HH':'mm':'ss");
  SYSTEMTIME end_of_day{};
  ::GetLocalTime(&end_of_day);
  end_of_day.wHour = 23;
  end_of_day.wMinute = 59;
  end_of_day.wSecond = 59;
  end_of_day.wMilliseconds = 0;
  DateTime_SetSystemtime(time, GDT_VALID, &end_of_day);

  add(WC_STATICW, L"", 0, kUtc);
  add(WC_BUTTONW, L"生成激活码", BS_DEFPUSHBUTTON | WS_TABSTOP, kGenerate);
  add(WC_STATICW, L"激活码", 0, kCodeLabel);
  add(WC_EDITW, L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_TABSTOP, kCode);
  add(WC_BUTTONW, L"复制激活码", WS_TABSTOP, kCopy);
  add(WC_STATICW, L"", 0, kStatus);
  add(WC_STATICW, L"公钥", 0, kKeyLabel);
  add(WC_EDITW, L"", WS_BORDER | ES_MULTILINE | ES_READONLY, kPublicKey);
  add(WC_STATICW, L"备份口令", 0, kPasswordLabel);
  add(WC_EDITW, L"", WS_BORDER | ES_PASSWORD | WS_TABSTOP, kPassword);
  add(WC_BUTTONW, L"备份私钥…", WS_TABSTOP, kBackup);
  add(WC_BUTTONW, L"从备份导入…", WS_TABSTOP, kRestore);
  add(WC_BUTTONW, L"签发记录", WS_TABSTOP, kRecords);
  add(WC_BUTTONW, L"生成新私钥", WS_TABSTOP, kCreateKey);
}

LRESULT CALLBACK window_procedure(const HWND window, const UINT message,
                                  const WPARAM wparam, const LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      g_window = window;
      create_controls();
      apply_font(::GetDpiForWindow(window));
      layout();
      refresh_utc();
      return 0;
    case WM_SIZE:
      layout();
      return 0;
    case WM_DPICHANGED: {
      const auto* const suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(window, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
      apply_font(HIWORD(wparam));
      layout();
      return 0;
    }
    case WM_NOTIFY: {
      const auto* const header = reinterpret_cast<const NMHDR*>(lparam);
      if ((header->idFrom == kDate || header->idFrom == kTime) &&
          header->code == DTN_DATETIMECHANGE) {
        refresh_utc();
      }
      return 0;
    }
    case WM_COMMAND:
      switch (LOWORD(wparam)) {
        case kZone:
          if (HIWORD(wparam) == CBN_SELCHANGE) refresh_utc();
          return 0;
        case kGenerate: run_guarded(generate); return 0;
        case kCopy: run_guarded(copy_code); return 0;
        case kBackup: run_guarded(backup); return 0;
        case kRestore: run_guarded(restore); return 0;
        case kRecords: run_guarded(open_records); return 0;
        case kCreateKey: run_guarded(create_key); return 0;
        default: break;
      }
      break;
    case WM_CTLCOLORSTATIC:
      // Read-only edits otherwise paint grey on grey under some themes.
      ::SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
      return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY:
      ::PostQuitMessage(0);
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

// Writes the public key as 128 hexadecimal digits for embedding in the
// application build: --export-public-key <path>. It never creates a key: a
// key is created only in the window, which asks for a backup at once.
int export_public_key(const wchar_t* const path) {
  try {
    auto signer = load_signer();
    if (!signer) return 2;
    const std::string text = narrow(hex_key(signer->public_key())) + "\r\n";
    replace_file(path, std::as_bytes(std::span(text.data(), text.size())));
    return 0;
  } catch (...) {
    return 1;
  }
}

}  // namespace

int WINAPI wWinMain(const HINSTANCE instance, HINSTANCE, PWSTR, const int show) {
  int argc = 0;
  LPWSTR* const argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (argv != nullptr && argc == 3 && std::wstring_view(argv[1]) == L"--export-public-key") {
    const int result = export_public_key(argv[2]);
    ::LocalFree(argv);
    return result;
  }
  if (argv != nullptr) ::LocalFree(argv);

  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_DATE_CLASSES | ICC_STANDARD_CLASSES};
  ::InitCommonControlsEx(&controls);

  try {
    g_signer = load_signer();
  } catch (const std::exception& error) {
    ::MessageBoxW(nullptr, (L"无法加载签名私钥：" + widen(error.what())).c_str(),
                  kTitle, MB_OK | MB_ICONERROR);
  }

  WNDCLASSEXW window_class{sizeof(window_class)};
  window_class.lpfnWndProc = window_procedure;
  window_class.hInstance = instance;
  window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = ::GetSysColorBrush(COLOR_WINDOW);
  window_class.lpszClassName = kWindowClass;
  ::RegisterClassExW(&window_class);

  const UINT dpi = ::GetDpiForSystem();
  const HWND window = ::CreateWindowExW(
      0, kWindowClass, kTitle, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT,
      CW_USEDEFAULT, ::MulDiv(640, static_cast<int>(dpi), 96),
      ::MulDiv(560, static_cast<int>(dpi), 96), nullptr, nullptr, instance, nullptr);
  if (window == nullptr) return 1;
  show_public_key();
  if (!g_signer) {
    set_status(L"尚未加载签名私钥：在“备份口令”中输入口令后点“从备份导入…”，或首次使用时点“生成新私钥”。");
  }
  ::ShowWindow(window, show);

  MSG message{};
  while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (!::IsDialogMessageW(window, &message)) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
  }
  g_signer.reset();
  if (g_font) ::DeleteObject(g_font);
  return static_cast<int>(message.wParam);
}
