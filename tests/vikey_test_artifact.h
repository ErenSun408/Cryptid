#pragma once

#include "card/vikey/vikey_artifact.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace usb_vault::card::vikey::test {

inline constexpr char kFixtureBytes[] = "usb-vault-vikey-test";
inline constexpr Sha256Digest kFixtureDigest{
    std::byte{0x1A}, std::byte{0x3D}, std::byte{0xF9}, std::byte{0x8B},
    std::byte{0xF1}, std::byte{0x49}, std::byte{0xA9}, std::byte{0x95},
    std::byte{0x54}, std::byte{0xCD}, std::byte{0xF5}, std::byte{0xA2},
    std::byte{0x1C}, std::byte{0xEA}, std::byte{0x93}, std::byte{0xBD},
    std::byte{0x6C}, std::byte{0x22}, std::byte{0xA8}, std::byte{0x5F},
    std::byte{0xCF}, std::byte{0x65}, std::byte{0x70}, std::byte{0x9D},
    std::byte{0x9D}, std::byte{0x8D}, std::byte{0x48}, std::byte{0x1C},
    std::byte{0xAE}, std::byte{0x6F}, std::byte{0x26}, std::byte{0xB7},
};

class TemporaryVikeyArtifact final {
 public:
  TemporaryVikeyArtifact() {
    std::array<wchar_t, MAX_PATH + 1> temp_path{};
    const DWORD path_length = ::GetTempPathW(
        static_cast<DWORD>(temp_path.size()), temp_path.data());
    if (path_length == 0 || path_length >= temp_path.size()) {
      throw_last_error("resolve temporary directory for ViKey test");
    }

    std::array<wchar_t, MAX_PATH + 1> unique_path{};
    if (::GetTempFileNameW(temp_path.data(), L"uvk", 0,
                           unique_path.data()) == 0) {
      throw_last_error("reserve unique ViKey test path");
    }
    if (::DeleteFileW(unique_path.data()) == FALSE) {
      throw_last_error("release unique ViKey test path");
    }

    root_ = std::wstring(unique_path.data()) + L".dir";
    directory_ = root_ / L"nested" / L"leaf";
    path_ = directory_ / L"ViKey.dll";
    try {
      std::filesystem::create_directories(directory_);
      std::ofstream output(path_, std::ios::binary | std::ios::trunc);
      output.write(kFixtureBytes,
                   static_cast<std::streamsize>(sizeof(kFixtureBytes) - 1));
      if (!output) {
        throw std::runtime_error("write ViKey verification test fixture");
      }
    } catch (...) {
      std::error_code ignored;
      std::filesystem::remove_all(root_, ignored);
      throw;
    }
  }

  ~TemporaryVikeyArtifact() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  TemporaryVikeyArtifact(const TemporaryVikeyArtifact&) = delete;
  TemporaryVikeyArtifact& operator=(const TemporaryVikeyArtifact&) = delete;
  TemporaryVikeyArtifact(TemporaryVikeyArtifact&&) = delete;
  TemporaryVikeyArtifact& operator=(TemporaryVikeyArtifact&&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

  [[nodiscard]] VerifiedVikeyDll verified() const {
    return verify_vikey_dll(directory_, kFixtureDigest);
  }

  void resize(const std::uintmax_t bytes) const {
    std::filesystem::resize_file(path_, bytes);
  }

  [[nodiscard]] bool can_open_for_write() const noexcept {
    return can_open(GENERIC_WRITE);
  }

  [[nodiscard]] bool can_open_for_delete() const noexcept {
    return can_open(DELETE);
  }

  [[nodiscard]] bool can_round_trip_rename_directory() noexcept {
    std::filesystem::path moved = directory_;
    moved += L".moved";
    if (::MoveFileExW(directory_.c_str(), moved.c_str(), 0) == FALSE) {
      return false;
    }
    if (::MoveFileExW(moved.c_str(), directory_.c_str(), 0) == FALSE) {
      directory_ = std::move(moved);
      path_ = directory_ / L"ViKey.dll";
      return false;
    }
    return true;
  }

  [[nodiscard]] bool can_round_trip_rename_ancestor() noexcept {
    const std::filesystem::path ancestor = directory_.parent_path();
    std::filesystem::path moved = ancestor;
    moved += L".moved";
    if (::MoveFileExW(ancestor.c_str(), moved.c_str(), 0) == FALSE) {
      return false;
    }
    if (::MoveFileExW(moved.c_str(), ancestor.c_str(), 0) == FALSE) {
      directory_ = moved / directory_.filename();
      path_ = directory_ / L"ViKey.dll";
      return false;
    }
    return true;
  }

 private:
  [[noreturn]] static void throw_last_error(const char* const context) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(), context);
  }

  [[nodiscard]] bool can_open(const DWORD desired_access) const noexcept {
    const HANDLE handle = ::CreateFileW(
        path_.c_str(), desired_access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return false;
    }
    static_cast<void>(::CloseHandle(handle));
    return true;
  }

  std::filesystem::path root_;
  std::filesystem::path directory_;
  std::filesystem::path path_;
};

}  // namespace usb_vault::card::vikey::test
