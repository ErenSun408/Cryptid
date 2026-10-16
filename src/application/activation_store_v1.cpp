#include "usb_vault/application/activation_store_v1.h"

#include <Windows.h>
#include <ShlObj.h>
#include <dpapi.h>

#include <charconv>
#include <stdexcept>
#include <utility>
#include <vector>

namespace usb_vault::application {
namespace {

constexpr std::string_view kHeader = "usb-vault activation v1\n";
constexpr std::string_view kHighestKey = "highest=";
constexpr std::string_view kCodeKey = "code=";
constexpr char kEntropy[] = "usb-vault activation store v1";
constexpr DWORD kMaximumFileBytes = 64 * 1024;
constexpr std::size_t kMaximumEntries = 64;
// Writing on every observation would rewrite the file on each page poll; a
// mark this coarse still exceeds nothing the rewind tolerance relies on.
constexpr std::int64_t kSaveIntervalSeconds = 60;

[[nodiscard]] DATA_BLOB entropy() noexcept {
  return {static_cast<DWORD>(sizeof(kEntropy) - 1),
          reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
}

enum class ReadOutcome { absent, read, unreadable, foreign };

// absent and foreign (not ours, damaged, or sealed for another account) may be
// replaced; unreadable means the file is there but could not be read now.
[[nodiscard]] ReadOutcome read_sealed(const std::wstring& path, std::string& text) {
  const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? ReadOutcome::absent : ReadOutcome::unreadable;
  }
  LARGE_INTEGER size{};
  std::vector<BYTE> sealed;
  if (::GetFileSizeEx(file, &size) == FALSE) {
    ::CloseHandle(file);
    return ReadOutcome::unreadable;
  }
  if (size.QuadPart <= 0 || size.QuadPart > kMaximumFileBytes) {
    ::CloseHandle(file);
    return ReadOutcome::foreign;
  }
  sealed.resize(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  const bool ok = ::ReadFile(file, sealed.data(), static_cast<DWORD>(sealed.size()),
                             &read, nullptr) != FALSE && read == sealed.size();
  ::CloseHandle(file);
  if (!ok) return ReadOutcome::unreadable;

  DATA_BLOB input{static_cast<DWORD>(sealed.size()), sealed.data()};
  DATA_BLOB salt = entropy();
  DATA_BLOB output{};
  if (!::CryptUnprotectData(&input, nullptr, &salt, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return ReadOutcome::foreign;
  }
  text.assign(reinterpret_cast<const char*>(output.pbData), output.cbData);
  ::LocalFree(output.pbData);
  return ReadOutcome::read;
}

void write_sealed(const std::wstring& path, const std::string& text) {
  DATA_BLOB input{static_cast<DWORD>(text.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(text.data()))};
  DATA_BLOB salt = entropy();
  DATA_BLOB output{};
  if (!::CryptProtectData(&input, L"usb-vault activation", &salt, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    throw std::runtime_error("DPAPI could not protect the activation record");
  }
  const std::size_t separator = path.find_last_of(L"\\/");
  if (separator != std::wstring::npos) {
    const std::wstring parent = path.substr(0, separator);
    if (!::CreateDirectoryW(parent.c_str(), nullptr) &&
        ::GetLastError() != ERROR_ALREADY_EXISTS) {
      ::LocalFree(output.pbData);
      throw std::runtime_error("cannot create the activation directory");
    }
  }
  const std::wstring temporary = path + L".tmp";
  const HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool ok = file != INVALID_HANDLE_VALUE;
  if (ok) {
    DWORD written = 0;
    ok = ::WriteFile(file, output.pbData, output.cbData, &written, nullptr) != FALSE &&
         written == output.cbData && ::FlushFileBuffers(file) != FALSE;
    ::CloseHandle(file);
  }
  ::LocalFree(output.pbData);
  if (!ok || !::MoveFileExW(temporary.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(temporary.c_str());
    throw std::runtime_error("cannot write the activation record");
  }
}

}  // namespace

ActivationStoreV1::ActivationStoreV1(std::wstring path,
                                     const licensing::ActivationPublicKey& key,
                                     Clock clock)
    : path_(std::move(path)), key_(key), clock_(std::move(clock)) {
  if (path_.empty() || !clock_) throw std::invalid_argument("invalid activation store");
}

std::int64_t ActivationStoreV1::system_utc_seconds() noexcept {
  FILETIME file{};
  ::GetSystemTimeAsFileTime(&file);
  const ULONGLONG ticks =
      (static_cast<ULONGLONG>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
  return static_cast<std::int64_t>(ticks / 10000000ULL) - 11644473600LL;
}

std::wstring ActivationStoreV1::default_path() {
  PWSTR folder = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY,
                                    nullptr, &folder)) || folder == nullptr) {
    if (folder != nullptr) ::CoTaskMemFree(folder);
    throw std::runtime_error("LocalAppData is unavailable");
  }
  std::wstring path = std::wstring(folder) + L"\\usb-vault\\activation.dat";
  ::CoTaskMemFree(folder);
  return path;
}

bool ActivationStoreV1::load_once() {
  if (loaded_) return true;
  std::string content;
  const ReadOutcome outcome = read_sealed(path_, content);
  if (outcome == ReadOutcome::unreadable) return false;
  loaded_ = true;
  if (outcome != ReadOutcome::read || !content.starts_with(kHeader)) return true;
  const std::string* const text = &content;
  std::size_t at = kHeader.size();
  while (at < text->size()) {
    std::size_t end = text->find('\n', at);
    if (end == std::string::npos) end = text->size();
    const std::string_view line(text->data() + at, end - at);
    at = end + 1;
    if (line.starts_with(kHighestKey)) {
      const std::string_view digits = line.substr(kHighestKey.size());
      std::int64_t value = 0;
      const auto [next, error] =
          std::from_chars(digits.data(), digits.data() + digits.size(), value);
      if (error == std::errc{} && next == digits.data() + digits.size() && value > 0) {
        highest_ = value;
      }
    } else if (line.starts_with(kCodeKey) && entries_.size() < kMaximumEntries) {
      // Every stored code is verified again; the record is not trusted for
      // anything the signature does not prove.
      const std::string code(line.substr(kCodeKey.size()));
      if (const auto grant = licensing::verify_activation_code(code, key_)) {
        entries_[grant->card_hardware_id] = {*grant, code};
      }
    }
  }
  saved_highest_ = highest_;
  return true;
}

std::int64_t ActivationStoreV1::observe() {
  const bool loaded = load_once();
  const std::int64_t now = clock_();
  if (!loaded) {
    // The record could not be read: judge against what this process has
    // seen, and leave the file alone.
    if (now > highest_) highest_ = now;
    return now;
  }
  if (now > highest_) highest_ = now;
  if (highest_ >= saved_highest_ + kSaveIntervalSeconds) {
    // An unwritable record must not lock out a valid grant; the mark is kept
    // in memory and the next interval tries again.
    try {
      save();
    } catch (...) {
      saved_highest_ = highest_;
    }
  }
  return now;
}

void ActivationStoreV1::save() {
  std::string text(kHeader);
  text += kHighestKey;
  text += std::to_string(highest_);
  text += '\n';
  for (const auto& [card, entry] : entries_) {
    text += kCodeKey;
    text += entry.code;
    text += '\n';
  }
  write_sealed(path_, text);
  saved_highest_ = highest_;
}

licensing::ActivationState ActivationStoreV1::state(
    const std::uint32_t card_hardware_id) noexcept {
  try {
    const std::lock_guard lock(mutex_);
    const std::int64_t now = observe();
    const auto found = entries_.find(card_hardware_id);
    const std::optional<licensing::ActivationGrant> grant =
        found == entries_.end() ? std::nullopt
                                : std::optional(found->second.grant);
    return licensing::evaluate_activation(grant, card_hardware_id, now, highest_);
  } catch (...) {
    return licensing::ActivationState::missing;
  }
}

std::optional<std::int64_t> ActivationStoreV1::expiry(
    const std::uint32_t card_hardware_id) noexcept {
  try {
    const std::lock_guard lock(mutex_);
    load_once();
    const auto found = entries_.find(card_hardware_id);
    if (found == entries_.end()) return std::nullopt;
    return found->second.grant.expires_at_utc_seconds;
  } catch (...) {
    return std::nullopt;
  }
}

ActivationInstallResult ActivationStoreV1::install(
    const std::uint32_t card_hardware_id, const std::string_view code) {
  // Anything longer than a grouped code with generous spacing is not one.
  if (code.size() > 512) return ActivationInstallResult::invalid;
  const auto grant = licensing::verify_activation_code(code, key_);
  if (!grant) return ActivationInstallResult::invalid;
  if (grant->card_hardware_id != card_hardware_id) {
    return ActivationInstallResult::other_card;
  }
  const std::lock_guard lock(mutex_);
  if (!load_once()) throw std::runtime_error("the activation record cannot be read now");
  const std::int64_t now = observe();
  switch (licensing::evaluate_activation(grant, card_hardware_id, now, highest_)) {
    case licensing::ActivationState::active: break;
    case licensing::ActivationState::clock_rewound:
      return ActivationInstallResult::clock_rewound;
    default:
      return ActivationInstallResult::expired;
  }
  // Keep the canonical spelling so the record never stores pasted noise.
  std::string canonical;
  for (const char c : code) {
    if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    canonical += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
  }
  if (entries_.size() >= kMaximumEntries && !entries_.contains(card_hardware_id)) {
    entries_.erase(entries_.begin());
  }
  const auto previous = entries_.find(card_hardware_id);
  const std::optional<Entry> restore =
      previous == entries_.end() ? std::nullopt : std::optional(previous->second);
  entries_[card_hardware_id] = {*grant, canonical};
  try {
    save();
  } catch (...) {
    if (restore) entries_[card_hardware_id] = *restore;
    else entries_.erase(card_hardware_id);
    throw;
  }
  return ActivationInstallResult::accepted;
}

}  // namespace usb_vault::application
