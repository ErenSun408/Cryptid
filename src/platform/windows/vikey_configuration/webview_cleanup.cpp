#include "webview_cleanup.h"
#include <Windows.h>
#include <ShlObj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wintrust.h>
#include <Softpub.h>
#include <wincrypt.h>
#include <array>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace configuration::webview_cleanup {
namespace {
using setup::Result;
constexpr wchar_t runtime_key[] = L"Software\\Microsoft\\EdgeUpdate\\Clients\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";
struct Handle {
  HANDLE value{INVALID_HANDLE_VALUE};
  explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
  ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
};
bool missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }
void require(bool ok) { if (!ok) throw std::runtime_error("unsafe or unavailable cleanup target"); }
bool equal(std::wstring_view a, std::wstring_view b) {
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
void plain_path(const std::filesystem::path& path) {
  const auto& text = path.native();
  require(text.size() > 3 && text.size() < 240 && text[1] == L':' && text[2] == L'\\' &&
          text.find_first_of(L"/\"*?", 2) == std::wstring::npos &&
          text.find(L':', 2) == std::wstring::npos && text.find(L'\0') == std::wstring::npos);
  require(GetDriveTypeW(path.root_path().c_str()) == DRIVE_FIXED);
  for (const auto& part : path.relative_path()) {
    const auto name = part.native();
    require(!name.empty() && name != L"." && name != L".." && name.back() != L'.' && name.back() != L' ');
  }
}
Handle open_plain(const std::filesystem::path& path, bool directory, bool remove) {
  const DWORD access = directory ? FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES : GENERIC_READ;
  Handle file(CreateFileW(path.c_str(), access | (remove ? DELETE : 0),
      directory ? FILE_SHARE_READ | FILE_SHARE_WRITE : FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
  require(file.value != INVALID_HANDLE_VALUE);
  FILE_ATTRIBUTE_TAG_INFO info{};
  require(GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &info, sizeof(info)) != FALSE);
  require(!(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
          ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory);
  if (remove && !directory) require(!(info.FileAttributes & FILE_ATTRIBUTE_READONLY));
  std::wstring final(32768, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(file.value, final.data(), static_cast<DWORD>(final.size()), FILE_NAME_NORMALIZED);
  require(length && length < final.size());
  final.resize(length);
  if (final.starts_with(L"\\\\?\\")) final.erase(0, 4);
  require(equal(final, path.native()));
  return file;
}
std::vector<Handle> pin_ancestry(const std::filesystem::path& path) {
  plain_path(path);
  std::vector<Handle> handles;
  auto current = path.root_path();
  handles.push_back(open_plain(current, true, false));
  for (const auto& part : path.relative_path()) {
    current /= part;
    handles.push_back(open_plain(current, true, false));
  }
  return handles;
}
std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
  PWSTR value{};
  require(SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &value)));
  std::filesystem::path path(value);
  CoTaskMemFree(value);
  return path;
}
std::optional<std::wstring> version(bool machine) {
  HKEY key{};
  const auto status = RegOpenKeyExW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, runtime_key, 0,
      KEY_QUERY_VALUE | (machine ? KEY_WOW64_32KEY : 0), &key);
  if (missing(static_cast<DWORD>(status))) return std::nullopt;
  require(status == ERROR_SUCCESS);
  struct Key { HKEY value; ~Key() { RegCloseKey(value); } } guard{key};
  std::array<wchar_t, 128> value{};
  DWORD size = static_cast<DWORD>(sizeof(value));
  const auto read = RegGetValueW(key, nullptr, L"pv", RRF_RT_REG_SZ | RRF_NOEXPAND, nullptr, value.data(), &size);
  if (read == ERROR_FILE_NOT_FOUND) return std::nullopt;
  require(read == ERROR_SUCCESS && size >= sizeof(wchar_t) && size <= sizeof(value) &&
          size % sizeof(wchar_t) == 0 && value[size / sizeof(wchar_t) - 1] == L'\0');
  std::wstring result(value.data(), size / sizeof(wchar_t) - 1);
  if (result.empty() || result == L"0.0.0.0") return std::nullopt;
  require(valid_version(result));
  return result;
}
bool idle() {
  Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  require(snapshot.value != INVALID_HANDLE_VALUE);
  PROCESSENTRY32W process{};
  process.dwSize = sizeof(process);
  require(Process32FirstW(snapshot.value, &process) != FALSE);
  do {
    if (equal(process.szExeFile, L"msedgewebview2.exe")) return false;
  } while (Process32NextW(snapshot.value, &process));
  require(GetLastError() == ERROR_NO_MORE_FILES);
  return true;
}
Result launch(const std::filesystem::path& executable, bool machine) {
  // Pin every ancestor and verified executable until ShellExecute has created
  // the process. Release before waiting so Microsoft's uninstaller can remove
  // its own files. No registry UninstallString is executed or concatenated.
  auto parents = pin_ancestry(executable.parent_path());
  auto file = open_plain(executable, false, false);
  require(microsoft_signed_file(executable));
  const auto working = executable.parent_path();
  SHELLEXECUTEINFOW action{};
  action.cbSize = sizeof(action);
  action.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  action.lpVerb = machine ? L"runas" : L"open";
  action.lpFile = executable.c_str();
  action.lpDirectory = working.c_str();
  action.lpParameters = machine ? L"--uninstall --msedgewebview --system-level" : L"--uninstall --msedgewebview";
  action.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&action)) return GetLastError() == ERROR_CANCELLED ? Result::cancelled : Result::failed;
  Handle process(action.hProcess);
  require(process.value != nullptr && process.value != INVALID_HANDLE_VALUE);
  CloseHandle(std::exchange(file.value, INVALID_HANDLE_VALUE));
  parents.clear();
  require(WaitForSingleObject(process.value, INFINITE) == WAIT_OBJECT_0);
  DWORD code{};
  require(GetExitCodeProcess(process.value, &code) != FALSE);
  // Edge setup reports uninstall success as 0. Unknown statuses never succeed.
  if (code == ERROR_CANCELLED || code == ERROR_INSTALL_USEREXIT) return Result::cancelled;
  return code == 0 ? Result::ready : Result::failed;
}
struct Entry { std::filesystem::path path; Handle handle; };
void collect(const std::filesystem::path& directory, std::vector<Entry>& entries, unsigned depth) {
  require(depth <= 64 && entries.size() < 20000);
  entries.push_back({directory, open_plain(directory, true, true)});
  WIN32_FIND_DATAW found{};
  const auto pattern = directory / L"*";
  const auto scan = FindFirstFileW(pattern.c_str(), &found);
  if (scan == INVALID_HANDLE_VALUE) { require(GetLastError() == ERROR_FILE_NOT_FOUND); return; }
  struct Scan { HANDLE value; ~Scan() { FindClose(value); } } guard{scan};
  do {
    const std::wstring_view name(found.cFileName);
    if (name == L"." || name == L"..") continue;
    require(name.find_first_of(L"\\/:*?\"") == std::wstring_view::npos && name.back() != L'.' && name.back() != L' ');
    require(!(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
    const auto child = directory / name;
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) collect(child, entries, depth + 1);
    else {
      require(entries.size() < 20000);
      entries.push_back({child, open_plain(child, false, true)});
    }
  } while (FindNextFileW(scan, &found));
  require(GetLastError() == ERROR_NO_MORE_FILES);
}
}

bool valid_version(std::wstring_view value) noexcept {
  unsigned parts = 0, digits = 0;
  if (value.empty() || value.size() > 43) return false;
  for (const wchar_t c : value) {
    if (c == L'.') { if (!digits || ++parts > 3) return false; digits = 0; }
    else if (c < L'0' || c > L'9' || ++digits > 10) return false;
  }
  return parts == 3 && digits != 0 && value != L"0.0.0.0";
}
std::filesystem::path setup_path(const std::filesystem::path& root, std::wstring_view value) {
  require(valid_version(value));
  plain_path(root);
  return root / L"Microsoft\\EdgeWebView\\Application" /
      value / L"Installer" / L"setup.exe";
}
bool microsoft_signed_file(const std::filesystem::path& path) noexcept {
  WINTRUST_FILE_INFO info{};
  info.cbStruct = sizeof(info);
  info.pcwszFilePath = path.c_str();
  WINTRUST_DATA data{};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &info;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const auto status = WinVerifyTrust(nullptr, &policy, &data);
  bool publisher = false;
  if (status == ERROR_SUCCESS) {
    const auto provider = WTHelperProvDataFromStateData(data.hWVTStateData);
    const auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
    if (signer && signer->csCertChain && signer->pasCertChain[0].pCert) {
      std::array<wchar_t, 256> name{};
      const auto count = CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
          nullptr, name.data(), static_cast<DWORD>(name.size()));
      publisher = count == 22 && std::wstring_view(name.data()) == L"Microsoft Corporation";
    }
  }
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &policy, &data);
  return status == ERROR_SUCCESS && publisher;
}
Result uninstall_runtime() noexcept {
  try {
    return remove_runtime([] {
      if (!version(false) && !version(true)) return Result::missing;
      return idle() ? Result::ready : Result::unavailable;
    }, [] {
      // Preflight both scopes before launching either, then verify current
      // registration again. Machine uninstall elevates only Microsoft's EXE.
      struct Target { bool machine; std::wstring version; std::filesystem::path path; };
      std::vector<Target> targets;
      for (bool machine : {false, true}) {
        if (const auto v = version(machine)) {
          const auto path = setup_path(known_folder(machine ? FOLDERID_ProgramFilesX86 : FOLDERID_LocalAppData), *v);
          auto parents = pin_ancestry(path.parent_path());
          auto file = open_plain(path, false, false);
          require(microsoft_signed_file(path));
          targets.push_back({machine, *v, path});
        }
      }
      for (const auto& target : targets) {
        const auto current = version(target.machine);
        require(current && *current == target.version && idle());
        const auto result = launch(target.path, target.machine);
        if (result != Result::ready) return result;
        require(!version(target.machine));
      }
      return Result::ready;
    }, [] { return !version(false) && !version(true); });
  } catch (...) { return Result::unavailable; }
}
Result clean_profile(const std::filesystem::path& application) noexcept {
  try {
    auto parents = pin_ancestry(application);
    const auto profile = application / L"USB Vault.exe.WebView2";
    const auto attributes = GetFileAttributesW(profile.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return missing(GetLastError()) ? Result::ready : Result::unavailable;
    auto profile_handle = open_plain(profile, true, true);
    const auto browser_data = profile / L"EBWebView";
    const auto browser_attributes = GetFileAttributesW(browser_data.c_str());
    std::vector<Entry> entries;
    if (browser_attributes != INVALID_FILE_ATTRIBUTES) collect(browser_data, entries, 0);
    else require(missing(GetLastError()));
    // Collect and lock the entire reserved application profile before any
    // deletion. A reparse point or active browser handle refuses the operation.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
      FILE_DISPOSITION_INFO remove{TRUE};
      require(SetFileInformationByHandle(it->handle.value, FileDispositionInfo, &remove, sizeof(remove)) != FALSE);
      CloseHandle(std::exchange(it->handle.value, INVALID_HANDLE_VALUE));
    }
    // Unknown siblings of EBWebView are self-stored files, not browser data.
    // Preserve them and allow uninstall to complete with this directory left.
    FILE_DISPOSITION_INFO remove{TRUE};
    if (!SetFileInformationByHandle(profile_handle.value, FileDispositionInfo, &remove, sizeof(remove)))
      return GetLastError() == ERROR_DIR_NOT_EMPTY ? Result::ready : Result::failed;
    return Result::ready;
  } catch (...) { return Result::unavailable; }
}
}
