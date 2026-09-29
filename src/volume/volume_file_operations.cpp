#include "usb_vault/volume/volume_file_operations.h"
#include "usb_vault/diagnostics/debug_log.h"

#include "volume/volume_containment.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
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

[[nodiscard]] std::uint64_t file_size_of(const WIN32_FIND_DATAW& found) noexcept {
  return (static_cast<std::uint64_t>(found.nFileSizeHigh) << 32) |
         static_cast<std::uint64_t>(found.nFileSizeLow);
}

[[nodiscard]] bool wants_stop(const TransferCancellation& cancelled) {
  return static_cast<bool>(cancelled) && cancelled();
}

[[noreturn]] void throw_cancelled() {
  throw std::system_error(static_cast<int>(ERROR_CANCELLED),
                          std::system_category(),
                          "the transfer was cancelled");
}

// 🔴 Neither of the two things a copy could do with a link is a copy.
// Following it reads whatever it names, which is off this volume and not what
// the user pointed at; recreating it writes a way out of the container into
// the container, which the rest of this layer then spends its time refusing.
// So a link in the source stops the operation instead.
void require_not_a_link(const DWORD attributes) {
  if (is_reparse_point(attributes)) {
    throw std::runtime_error("a link cannot be copied");
  }
}

// Adds up what one directory holds, refusing any link on the way.
[[nodiscard]] std::uint64_t measure_directory_tree(
    const std::wstring& directory, const TransferCancellation& cancelled) {
  const std::wstring pattern = directory + L"\\*";
  WIN32_FIND_DATAW found{};
  const FindHandle find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic,
                                           &found, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH));
  if (find.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return 0;
    throw_last_error(error, "list the directory being copied");
  }

  std::uint64_t total = 0;
  do {
    if (is_dot_entry(found.cFileName)) continue;
    if (wants_stop(cancelled)) throw_cancelled();
    require_not_a_link(found.dwFileAttributes);
    total += is_directory(found.dwFileAttributes)
                 ? measure_directory_tree(directory + L"\\" + found.cFileName,
                                          cancelled)
                 : file_size_of(found);
  } while (::FindNextFileW(find.get(), &found) != FALSE);

  const DWORD error = ::GetLastError();
  if (error != ERROR_NO_MORE_FILES && error != 0) {
    throw_last_error(error, "finish listing the directory being copied");
  }
  return total;
}

struct SourceSurvey final {
  bool is_directory{};
  std::uint64_t total_bytes{};
};

// One walk of the source before a single byte is written. It settles how much
// room the copy needs and refuses every link in it, and doing both here is why
// neither answer can leave a half-copied tree to undo.
[[nodiscard]] SourceSurvey survey_source(
    const std::wstring& source, const TransferCancellation& cancelled) {
  DWORD attributes = 0;
  std::uint64_t size = 0;
  {
    WIN32_FIND_DATAW found{};
    const FindHandle find(::FindFirstFileW(source.c_str(), &found));
    if (find.get() == INVALID_HANDLE_VALUE) {
      throw_last_error(::GetLastError(), "inspect the entry being copied");
    }
    attributes = found.dwFileAttributes;
    size = file_size_of(found);
  }

  require_not_a_link(attributes);
  if (!is_directory(attributes)) {
    return SourceSurvey{.is_directory = false, .total_bytes = size};
  }
  return SourceSurvey{.is_directory = true,
                      .total_bytes = measure_directory_tree(source, cancelled)};
}

// Free space belongs to the volume rather than to any directory on it, so the
// drive root is both the right thing to ask about and one that already exists
// when the destination does not. The answer is what this account may use,
// which is what a quota would cut down, and that is the room the copy has.
void require_room_on_volume(const wchar_t drive, const std::uint64_t needed) {
  const std::wstring root{drive, L':', L'\\'};
  ULARGE_INTEGER available{};
  if (::GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) ==
      FALSE) {
    throw_last_error(::GetLastError(), "measure the room on the destination");
  }
  if (available.QuadPart < needed) {
    throw std::system_error(static_cast<int>(ERROR_DISK_FULL),
                            std::system_category(),
                            "the destination has no room for the copy");
  }
}

// Asked by `CopyFileEx` as it writes. `PROGRESS_CANCEL` rather than
// `PROGRESS_STOP` on purpose: cancel makes it delete the partly written
// destination file itself, stop leaves the fragment sitting there.
struct CopyProgress {
  const TransferCancellation& cancelled;
  bool destination_opened{};
};

DWORD CALLBACK on_copy_progress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER,
                                LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE,
                                const LPVOID data) {
  auto* const progress = static_cast<CopyProgress*>(data);
  progress->destination_opened = true;
  // An exception must not cross back into the file system's own frames, and a
  // cancellation that misbehaves is reason enough to stop.
  try {
    return wants_stop(progress->cancelled) ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("volume/volume_file_operations", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    return PROGRESS_CANCEL;
  }
}

// Takes away what this copy created. Best-effort on purpose: the failure that
// brought us here is the one worth reporting, and a rollback that cannot run
// does not make it more useful to hear about.
void discard_partial_destination(const std::wstring& path) noexcept {
  try {
    delete_volume_entry(path);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("volume/volume_file_operations", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
  }
}

void copy_file_entry(const std::wstring& source,
                     const std::wstring& destination,
                     const TransferCancellation& cancelled) {
  CopyProgress progress{cancelled};
  if (::CopyFileExW(source.c_str(), destination.c_str(), &on_copy_progress,
                    &progress, nullptr, COPY_FILE_FAIL_IF_EXISTS) != FALSE) {
    return;
  }

  const DWORD error = ::GetLastError();
  if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
    // 🔴 Not this operation's to take away. The entry standing in the way is
    // the user's, and nothing was written over it.
    throw_last_error(error, "copy the file");
  }
  // Never clean up an occupied path that CopyFileEx did not open for copying.
  if (progress.destination_opened) discard_partial_destination(destination);
  if (error == ERROR_REQUEST_ABORTED) throw_cancelled();
  throw_last_error(error, "copy the file");
}

// Copies what is inside one directory into another that already exists. The
// survey refused every link in the tree before this started; the attributes
// are read again here because the source is a mounted volume and nothing stops
// one appearing in between.
void copy_directory_contents(const std::wstring& source,
                             const std::wstring& destination,
                             const TransferCancellation& cancelled) {
  const std::wstring pattern = source + L"\\*";
  WIN32_FIND_DATAW found{};
  const FindHandle find(::FindFirstFileExW(pattern.c_str(), FindExInfoBasic,
                                           &found, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH));
  if (find.get() == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return;
    throw_last_error(error, "list the directory being copied");
  }

  do {
    if (is_dot_entry(found.cFileName)) continue;
    if (wants_stop(cancelled)) throw_cancelled();
    require_not_a_link(found.dwFileAttributes);
    const std::wstring child_source = source + L"\\" + found.cFileName;
    const std::wstring child_destination =
        destination + L"\\" + found.cFileName;
    if (!is_directory(found.dwFileAttributes)) {
      copy_file_entry(child_source, child_destination, cancelled);
      continue;
    }
    if (::CreateDirectoryW(child_destination.c_str(), nullptr) == FALSE) {
      throw_last_error(::GetLastError(), "create a directory being copied");
    }
    copy_directory_contents(child_source, child_destination, cancelled);
  } while (::FindNextFileW(find.get(), &found) != FALSE);

  const DWORD error = ::GetLastError();
  if (error != ERROR_NO_MORE_FILES && error != 0) {
    throw_last_error(error, "finish listing the directory being copied");
  }
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

void copy_volume_entry(const std::wstring_view absolute_source,
                       const std::wstring_view absolute_destination,
                       const TransferCancellation& cancelled,
                       const bool rename_on_collision) {
  const wchar_t source_drive = require_drive_absolute(absolute_source);
  const wchar_t destination_drive = require_drive_absolute(absolute_destination);

  const std::wstring source(absolute_source);
  const std::wstring destination(absolute_destination);
  // Only a question on one volume. Across two, neither path is inside the
  // other whatever the text looks like.
  if (upper_ascii(source_drive) == upper_ascii(destination_drive) &&
      is_same_or_inside(source, destination)) {
    throw std::invalid_argument("an entry cannot be copied inside itself");
  }

  require_parent_stays_on_volume(source, source_drive);
  require_parent_stays_on_volume(destination, destination_drive);

  const SourceSurvey survey = survey_source(source, cancelled);
  require_room_on_volume(destination_drive, survey.total_bytes);
  if (wants_stop(cancelled)) throw_cancelled();

  std::wstring target = destination;
  const std::filesystem::path path(destination);
  const std::wstring extension = survey.is_directory ? L"" : path.extension().wstring();
  const std::wstring stem = survey.is_directory ? path.filename().wstring() : path.stem().wstring();
  for (unsigned int number = 0; ; ++number) {
    if (wants_stop(cancelled)) throw_cancelled();
    if (number > 100000) throw_last_error(ERROR_FILE_EXISTS, "too many export name collisions");
    if (number != 0) target = (path.parent_path() / (stem + L" (" + std::to_wstring(number) + L")" + extension)).wstring();
    if (rename_on_collision && ::GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
    if (!survey.is_directory) {
      try { copy_file_entry(source, target, cancelled); return; }
      catch (const std::system_error& error) {
        if (rename_on_collision && error.code().category() == std::system_category() &&
            (error.code().value() == ERROR_FILE_EXISTS || error.code().value() == ERROR_ALREADY_EXISTS)) continue;
        throw;
      }
    }
    if (::CreateDirectoryW(target.c_str(), nullptr) != FALSE) break;
    const DWORD error = ::GetLastError();
    if (rename_on_collision && (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS)) continue;
    throw_last_error(error, "create the destination directory");
  }

  // 🔴 Made before the rollback scope opens, deliberately. If this fails
  // because something is already there, that something is the user's, and a
  // rollback must never be in a position to take it away.
  try {
    copy_directory_contents(source, target, cancelled);
  } catch (...) {
    discard_partial_destination(target);
    throw;
  }
}

void move_volume_entry_by_copying(const std::wstring_view absolute_source,
                                  const std::wstring_view absolute_destination,
                                  const TransferCancellation& cancelled) {
  copy_volume_entry(absolute_source, absolute_destination, cancelled);
  // Only once the copy is whole. A cancelled or failed copy has already taken
  // its own leavings away and thrown, so the source is never removed against a
  // destination in doubt.
  delete_volume_entry(absolute_source);
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
