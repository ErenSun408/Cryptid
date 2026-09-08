#include "usb_vault/volume/volume_file_operations.h"

#include "volume/volume_containment.h"

#include <Windows.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace usb_vault::volume {
namespace {

[[noreturn]] void throw_last_error(const DWORD error,
                                   const char* const operation) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          operation);
}

class FindHandle final {
 public:
  explicit FindHandle(const HANDLE handle) noexcept : handle_(handle) {}
  ~FindHandle() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) ::FindClose(handle_);
  }
  FindHandle(const FindHandle&) = delete;
  FindHandle& operator=(const FindHandle&) = delete;
  FindHandle(FindHandle&&) = delete;
  FindHandle& operator=(FindHandle&&) = delete;
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }

 private:
  HANDLE handle_;
};

[[nodiscard]] bool is_dot_entry(const wchar_t* const name) noexcept {
  return name[0] == L'.' &&
         (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

// The attributes of the entry itself, never of what a link points at.
// `FindFirstFileW` answers about the name; `GetFileAttributesW` can be made to
// answer about the target, which is the wrong question everywhere below.
[[nodiscard]] DWORD entry_attributes(const std::wstring& path) {
  WIN32_FIND_DATAW found{};
  const FindHandle find(::FindFirstFileW(path.c_str(), &found));
  if (find.get() == INVALID_HANDLE_VALUE) {
    throw_last_error(::GetLastError(), "inspect the entry");
  }
  return found.dwFileAttributes;
}

[[nodiscard]] bool is_directory(const DWORD attributes) noexcept {
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

[[nodiscard]] bool is_reparse_point(const DWORD attributes) noexcept {
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Read-only is an attribute rather than a permission on Windows, and the
// caller has already decided the entry goes. Cleared on this entry alone.
void clear_read_only(const std::wstring& path, const DWORD attributes) {
  if ((attributes & FILE_ATTRIBUTE_READONLY) == 0) return;
  ::SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
}

void delete_file_entry(const std::wstring& path, const DWORD attributes) {
  clear_read_only(path, attributes);
  if (::DeleteFileW(path.c_str()) == FALSE) {
    throw_last_error(::GetLastError(), "delete the file");
  }
}

void remove_directory_entry(const std::wstring& path, const DWORD attributes) {
  clear_read_only(path, attributes);
  if (::RemoveDirectoryW(path.c_str()) == FALSE) {
    throw_last_error(::GetLastError(), "remove the directory");
  }
}

// Deletes the contents of one directory and then the directory. `directory` is
// known to be a real directory and not a reparse point when this is called,
// and the walk keeps that true for every level it enters.
void delete_directory_tree(const std::wstring& directory) {
  const std::wstring pattern = directory + L"\\*";
  WIN32_FIND_DATAW found{};
  const FindHandle find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic,
                                           &found, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH));
  if (find.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND) {
      throw_last_error(error, "list the directory being deleted");
    }
  } else {
    do {
      if (is_dot_entry(found.cFileName)) continue;
      const std::wstring child = directory + L"\\" + found.cFileName;
      if (!is_directory(found.dwFileAttributes)) {
        delete_file_entry(child, found.dwFileAttributes);
      } else if (is_reparse_point(found.dwFileAttributes)) {
        // 🔴 The link goes, its target does not. Descending here is how a
        // delete inside a container empties a directory somewhere else on the
        // machine, and `RemoveDirectoryW` on a junction removes only the link.
        remove_directory_entry(child, found.dwFileAttributes);
      } else {
        delete_directory_tree(child);
      }
    } while (::FindNextFileW(find.get(), &found) != FALSE);

    const DWORD error = ::GetLastError();
    if (error != ERROR_NO_MORE_FILES && error != 0) {
      throw_last_error(error, "finish listing the directory being deleted");
    }
  }

  const DWORD attributes = entry_attributes(directory);
  remove_directory_entry(directory, attributes);
}

[[nodiscard]] bool equals_ignoring_case(const std::wstring_view left,
                                        const std::wstring_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (upper_ascii(left[index]) != upper_ascii(right[index])) return false;
  }
  return true;
}

// Whether `inner` names the same entry as `outer` or something beneath it.
// Compared as text, which is sound here because both paths came out of
// `resolve_volume_entry_path`: neither can hold `.`, `..`, a doubled
// separator, or a forward slash, so one is a prefix of the other exactly when
// it is an ancestor of it.
[[nodiscard]] bool is_same_or_inside(const std::wstring& outer,
                                     const std::wstring& inner) noexcept {
  if (equals_ignoring_case(outer, inner)) return true;
  if (inner.size() <= outer.size()) return false;
  if (!equals_ignoring_case(std::wstring_view(inner).substr(0, outer.size()),
                            outer)) {
    return false;
  }
  // `X:\workshop` must not read as inside `X:\work`.
  return inner[outer.size()] == L'\\';
}

}  // namespace

void delete_volume_entry(const std::wstring_view absolute_path) {
  const wchar_t drive = require_drive_absolute(absolute_path);
  const std::wstring path(absolute_path);
  require_parent_stays_on_volume(path, drive);

  const DWORD attributes = entry_attributes(path);
  if (!is_directory(attributes)) {
    delete_file_entry(path, attributes);
    return;
  }
  if (is_reparse_point(attributes)) {
    remove_directory_entry(path, attributes);
    return;
  }
  delete_directory_tree(path);
}

void move_volume_entry(const std::wstring_view absolute_source,
                       const std::wstring_view absolute_destination) {
  const wchar_t source_drive = require_drive_absolute(absolute_source);
  const wchar_t destination_drive = require_drive_absolute(absolute_destination);
  if (upper_ascii(source_drive) != upper_ascii(destination_drive)) {
    throw std::invalid_argument("a move across volumes is a copy and a delete");
  }

  const std::wstring source(absolute_source);
  const std::wstring destination(absolute_destination);
  if (is_same_or_inside(source, destination)) {
    throw std::invalid_argument("an entry cannot move inside itself");
  }

  require_parent_stays_on_volume(source, source_drive);
  require_parent_stays_on_volume(destination, destination_drive);

  // Without `MOVEFILE_REPLACE_EXISTING`, so an occupied destination fails
  // instead of taking a file the user never named with it.
  if (::MoveFileExW(source.c_str(), destination.c_str(), 0) == FALSE) {
    throw_last_error(::GetLastError(), "move the entry");
  }
}

void create_volume_directory(const std::wstring_view absolute_path) {
  const wchar_t drive = require_drive_absolute(absolute_path);
  const std::wstring path(absolute_path);
  require_parent_stays_on_volume(path, drive);

  if (::CreateDirectoryW(path.c_str(), nullptr) == FALSE) {
    throw_last_error(::GetLastError(), "create the directory");
  }
}

}  // namespace usb_vault::volume
