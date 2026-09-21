#include "volume_creation/import_unique_name.h"
#include <filesystem>
#include <iostream>
#include <vector>
using usb_vault::volume_creation::detail::create_unique_import;
void check(bool value) { if (!value) throw std::runtime_error("unique import assertion failed"); }
int main() {
  try {
    auto root = std::filesystem::temp_directory_path() / (L"usb-vault-name-" + std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64()));
    check(::CreateDirectoryW(root.c_str(), nullptr) != 0);
    std::vector<std::filesystem::path> owned;
    const auto create = [&](const std::wstring& name) {
      auto path = root / name;
      HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
      if (file == INVALID_HANDLE_VALUE) throw std::system_error(::GetLastError(), std::system_category());
      DWORD written=0;
      check(::WriteFile(file, "keep", 4, &written, nullptr) && written==4);
      ::CloseHandle(file); owned.push_back(path); return name;
    };
    check(create_unique_import(L"photo.jpg", false, create) == L"photo.jpg");
    check(create_unique_import(L"photo.jpg", false, create) == L"photo (1).jpg");
    check(create_unique_import(L"PHOTO.jpg", false, create) == L"PHOTO (2).jpg");
    check(owned.size() == 3 && std::filesystem::exists(root / L"photo.jpg"));
    check(std::filesystem::file_size(root / L"photo.jpg") == 4);
    const auto make_directory = [&](const std::wstring& name) {
      if (!::CreateDirectoryW((root / name).c_str(), nullptr))
        throw std::system_error(::GetLastError(), std::system_category());
      return name;
    };
    check(create_unique_import(L"folder.v1", true, make_directory) == L"folder.v1");
    check(create_unique_import(L"folder.v1", true, make_directory) == L"folder.v1 (1)");
    check(::RemoveDirectoryW((root / L"folder.v1 (1)").c_str()) != 0);
    check(::RemoveDirectoryW((root / L"folder.v1").c_str()) != 0);
    unsigned attempts = 0;
    try {
      create_unique_import(L"a.txt", false, [&](const std::wstring&) -> bool {
        ++attempts; throw std::system_error(ERROR_ACCESS_DENIED, std::system_category());
      });
      check(false);
    } catch (const std::system_error& error) { check(error.code().value() == ERROR_ACCESS_DENIED && attempts == 1); }
    const auto after_collision = [](std::wstring original, bool directory) {
      unsigned count=0;
      return create_unique_import(original, directory, [&](const std::wstring& candidate) {
        if (!count++) throw std::system_error(ERROR_ALREADY_EXISTS, std::system_category());
        return candidate;
      });
    };
    check(after_collision(L"folder.v1", true) == L"folder.v1 (1)");
    check(after_collision(L"README", false) == L"README (1)");
    check(after_collision(L".config", false) == L".config (1)");
    const auto long_name = after_collision(std::wstring(251,L'a')+L".txt",false);
    check(long_name.size()==255 && long_name.ends_with(L" (1).txt"));
    for (const auto& path : owned) check(::DeleteFileW(path.c_str()) != 0);
    check(::RemoveDirectoryW(root.c_str()) != 0);
    std::cout << "import_unique_name_tests passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
