#include "usb_vault/volume/volume_file_operations.h"

#include "volume_test_support.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace {

using usb_vault::volume::copy_volume_entry;
using usb_vault::volume::create_volume_directory;
using usb_vault::volume::delete_volume_entry;
using usb_vault::volume::move_volume_entry;
using usb_vault::volume::move_volume_entry_by_copying;
using usb_vault_test::count_files_below;
using usb_vault_test::ExclusiveFile;
using usb_vault_test::path_exists;
using usb_vault_test::make_directory;
using usb_vault_test::make_file;
using usb_vault_test::read_text_file;
using usb_vault_test::SubstitutedDrive;
using usb_vault_test::TemporaryDirectory;
using usb_vault_test::try_make_junction;
using usb_vault_test::try_make_sparse_file;
using usb_vault_test::write_text_file;

int g_failures = 0;

void require(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++g_failures;
  }
}

template <typename Callable>
void require_throws(Callable&& callable, const char* const message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::exception&) {
    return;
  }
  require(false, message);
}

// Sharper than `require_throws` where the caller has to be able to tell the
// outcomes apart: a page that must say "there is already something called
// that" cannot act on "some exception happened".
template <typename Callable>
void require_win32_error(Callable&& callable, const DWORD expected,
                         const char* const message) {
  try {
    std::forward<Callable>(callable)();
  } catch (const std::system_error& error) {
    if (error.code().category() == std::system_category() &&
        error.code().value() == static_cast<int>(expected)) {
      return;
    }
    std::cerr << "  (got " << error.code().value() << ": " << error.what()
              << ")\n";
    require(false, message);
    return;
  } catch (const std::exception& error) {
    std::cerr << "  (got " << error.what() << ")\n";
    require(false, message);
    return;
  }
  require(false, message);
}

void test_numbered_exports_preserve_existing_entries() {
  const TemporaryDirectory temporary;
  const auto source = temporary.root() / L"source.txt";
  const auto target = temporary.root() / L"report.txt";
  write_text_file(source, "new");
  write_text_file(target, "original");
  write_text_file(temporary.root() / L"report (1).txt", "previous");
  copy_volume_entry(source.wstring(), target.wstring(), {}, true);
  require(read_text_file(target) == "original", "export overwrote original");
  require(read_text_file(temporary.root() / L"report (1).txt") == "previous", "export overwrote numbered file");
  require(read_text_file(temporary.root() / L"report (2).txt") == "new", "export did not preserve extension");
  const auto folder = temporary.root() / L"folder.with.dot";
  make_directory(folder);
  write_text_file(folder / L"inside.txt", "nested");
  const auto output = temporary.root() / L"output";
  make_directory(output);
  make_directory(output / L"folder.with.dot");
  copy_volume_entry(folder.wstring(), (output / L"folder.with.dot").wstring(), {}, true);
  require(read_text_file(output / L"folder.with.dot (1)" / L"inside.txt") == "nested", "directory suffix treated dot as extension");
  require(!path_exists(output / L"folder.with.dot" / L"inside.txt"), "export merged directories");
  make_directory(output / L"source.txt");
  copy_volume_entry(source.wstring(), (output / L"source.txt").wstring(), {}, true);
  require(path_exists(output / L"source.txt") && read_text_file(output / L"source (1).txt") == "new", "file-directory collision lost data");
  require_throws([&] { copy_volume_entry(source.wstring(), target.wstring(), [] { return true; }, true); }, "cancelled export copied anyway");
  require(!path_exists(temporary.root() / L"report (3).txt"), "cancelled export left a copy");
}

void test_a_file_is_deleted() {
  const TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.root() / L"notes.txt";
  make_file(file, 5);
  require(path_exists(file), "the fixture file was not created");

  delete_volume_entry(file.wstring());
  require(!path_exists(file), "the file was not deleted");
}

// Read-only on Windows is an attribute, not a permission, and the caller has
// already decided the entry goes.
void test_a_read_only_file_is_deleted() {
  const TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.root() / L"locked.txt";
  make_file(file, 3);
  ::SetFileAttributesW(file.wstring().c_str(), FILE_ATTRIBUTE_READONLY);

  delete_volume_entry(file.wstring());
  require(!path_exists(file), "a read-only file survived deletion");
}

void test_a_directory_goes_with_everything_inside_it() {
  const TemporaryDirectory temporary;
  const std::filesystem::path tree = temporary.root() / L"tree";
  make_directory(tree / L"deep" / L"deeper");
  make_file(tree / L"top.txt", 1);
  make_file(tree / L"deep" / L"middle.txt", 2);
  make_file(tree / L"deep" / L"deeper" / L"bottom.txt", 3);
  ::SetFileAttributesW((tree / L"deep" / L"middle.txt").wstring().c_str(),
                       FILE_ATTRIBUTE_READONLY);

  delete_volume_entry(tree.wstring());
  require(!path_exists(tree), "the directory tree was not deleted");
  require(path_exists(temporary.root()), "the parent directory was deleted too");
}

// 🔴 The one that matters. A junction inside the volume can name any directory
// on the machine, so a recursive delete that descended into one would empty
// somewhere the user never named.
void test_a_recursive_delete_unlinks_a_junction_and_spares_its_target() {
  const TemporaryDirectory temporary;
  const std::filesystem::path outside = temporary.root() / L"outside";
  make_directory(outside);
  make_file(outside / L"precious.txt", 9);

  const std::filesystem::path tree = temporary.root() / L"tree";
  make_directory(tree);
  make_file(tree / L"ordinary.txt", 1);
  if (!try_make_junction(tree / L"link", outside)) {
    std::cout << "  (skipped: this machine would not create a junction)\n";
    return;
  }
  require(path_exists(tree / L"link" / L"precious.txt"),
          "the junction does not reach its target");

  delete_volume_entry(tree.wstring());

  require(!path_exists(tree), "the directory tree was not deleted");
  require(path_exists(outside), "the junction target was deleted with the tree");
  require(path_exists(outside / L"precious.txt"),
          "a recursive delete walked through a junction and emptied it");
}

// Deleting the link itself is an ordinary delete of one entry, and it must
// still leave the target alone.
void test_deleting_a_junction_leaves_its_target() {
  const TemporaryDirectory temporary;
  const std::filesystem::path outside = temporary.root() / L"outside";
  make_directory(outside);
  make_file(outside / L"precious.txt", 4);

  const std::filesystem::path link = temporary.root() / L"link";
  if (!try_make_junction(link, outside)) {
    std::cout << "  (skipped: this machine would not create a junction)\n";
    return;
  }

  delete_volume_entry(link.wstring());
  require(!path_exists(link), "the junction was not removed");
  require(path_exists(outside / L"precious.txt"),
          "removing a junction took its target with it");
}

void test_an_entry_reached_through_an_escaping_parent_is_refused() {
  const TemporaryDirectory temporary;
  make_file(temporary.root() / L"victim.txt", 2);
  const SubstitutedDrive substituted(temporary.path());
  if (!substituted.available()) {
    std::cout << "  (skipped: no free drive letter on this machine)\n";
    return;
  }

  require_throws(
      [&] { delete_volume_entry(substituted.child(L"victim.txt")); },
      "an entry whose parent resolves onto another volume was deleted");
  require(path_exists(temporary.root() / L"victim.txt"),
          "the refused delete removed the file anyway");
}

void test_renaming_and_moving_are_the_same_operation() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"before.txt";
  make_file(source, 6);

  // A rename is a move whose parents are equal.
  const std::filesystem::path renamed = temporary.root() / L"after.txt";
  move_volume_entry(source.wstring(), renamed.wstring());
  require(!path_exists(source) && path_exists(renamed), "the entry was not renamed");

  const std::filesystem::path folder = temporary.root() / L"folder";
  make_directory(folder);
  const std::filesystem::path moved = folder / L"after.txt";
  move_volume_entry(renamed.wstring(), moved.wstring());
  require(!path_exists(renamed) && path_exists(moved), "the entry was not moved");
}

void test_a_move_never_replaces_what_is_already_there() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"source.txt";
  const std::filesystem::path occupied = temporary.root() / L"occupied.txt";
  make_file(source, 1);
  make_file(occupied, 40);

  require_throws([&] { move_volume_entry(source.wstring(), occupied.wstring()); },
                 "a move replaced an existing entry");
  require(path_exists(source), "the refused move removed the source");
  require(std::filesystem::file_size(occupied) == 40,
          "the refused move overwrote the destination");
}

void test_a_directory_cannot_move_inside_itself() {
  const TemporaryDirectory temporary;
  const std::filesystem::path tree = temporary.root() / L"tree";
  make_directory(tree / L"inner");

  require_throws(
      [&] {
        move_volume_entry(tree.wstring(), (tree / L"inner" / L"tree").wstring());
      },
      "a directory was moved into its own subtree");
  require_throws([&] { move_volume_entry(tree.wstring(), tree.wstring()); },
                 "a directory was moved onto itself");
  require(path_exists(tree / L"inner"), "the refused move disturbed the tree");

  // A sibling whose name merely starts the same is not inside it.
  const std::filesystem::path sibling = temporary.root() / L"treehouse";
  make_directory(sibling);
  const std::filesystem::path destination = tree / L"treehouse";
  move_volume_entry(sibling.wstring(), destination.wstring());
  require(path_exists(destination), "a same-prefix sibling was refused as a child");
}

void test_a_move_across_volumes_is_refused_here() {
  const TemporaryDirectory temporary;
  make_file(temporary.root() / L"source.txt", 1);
  const SubstitutedDrive substituted(temporary.path());
  if (!substituted.available()) {
    std::cout << "  (skipped: no free drive letter on this machine)\n";
    return;
  }
  require_throws(
      [&] {
        move_volume_entry(temporary.child(L"source.txt"),
                          substituted.child(L"source.txt"));
      },
      "a move across drive letters was accepted");
}

void test_a_directory_is_created_but_never_a_chain_of_them() {
  const TemporaryDirectory temporary;
  const std::filesystem::path folder = temporary.root() / L"folder";
  create_volume_directory(folder.wstring());
  require(path_exists(folder), "the directory was not created");

  require_throws(
      [&] {
        create_volume_directory((temporary.root() / L"a" / L"b").wstring());
      },
      "a missing parent was created on the way");
  require(!path_exists(temporary.root() / L"a"),
          "a chain of directories was created");

  require_throws([&] { create_volume_directory(folder.wstring()); },
                 "an existing directory was created again");
}

void test_a_path_that_is_not_drive_absolute_is_refused() {
  require_throws([] { delete_volume_entry(L"relative\\path"); },
                 "a relative path was deleted");
  require_throws([] { delete_volume_entry(L"\\\\server\\share\\file"); },
                 "a UNC path was deleted");
  require_throws([] { move_volume_entry(L"relative", L"X:\\other"); },
                 "a relative source was moved");
  require_throws([] { copy_volume_entry(L"relative", L"X:\\other"); },
                 "a relative source was copied");
  require_throws([] { copy_volume_entry(L"X:\\source", L"\\\\server\\share\\f"); },
                 "a UNC destination was copied to");
}

void test_a_missing_entry_is_reported_rather_than_ignored() {
  const TemporaryDirectory temporary;
  require_throws([&] { delete_volume_entry(temporary.child(L"absent.txt")); },
                 "deleting a missing entry succeeded");
  require_throws(
      [&] {
        move_volume_entry(temporary.child(L"absent.txt"),
                          temporary.child(L"elsewhere.txt"));
      },
      "moving a missing entry succeeded");
}

void test_a_file_is_copied_and_the_source_stays() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"notes.txt";
  const std::filesystem::path destination =
      temporary.root() / L"notes-copy.txt";
  write_text_file(source, "the contents");

  copy_volume_entry(source.wstring(), destination.wstring());
  require(read_text_file(destination) == "the contents",
          "the copy does not hold what the source held");
  require(read_text_file(source) == "the contents",
          "the copy disturbed the source");
}

void test_a_directory_is_copied_with_everything_inside_it() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"tree";
  make_directory(source / L"deep" / L"deeper");
  make_directory(source / L"empty");
  write_text_file(source / L"top.txt", "top");
  write_text_file(source / L"deep" / L"middle.txt", "middle");
  write_text_file(source / L"deep" / L"deeper" / L"bottom.txt", "bottom");

  const std::filesystem::path destination = temporary.root() / L"tree-copy";
  copy_volume_entry(source.wstring(), destination.wstring());

  require(read_text_file(destination / L"top.txt") == "top",
          "the copied tree lost its top file");
  require(read_text_file(destination / L"deep" / L"middle.txt") == "middle",
          "the copied tree lost a file one level down");
  require(read_text_file(destination / L"deep" / L"deeper" / L"bottom.txt") ==
              "bottom",
          "the copied tree lost its deepest file");
  require(path_exists(destination / L"empty"),
          "the copied tree lost a directory that held nothing");
  require(count_files_below(source) == 3, "the copy changed the source");
  require(count_files_below(destination) == 3,
          "the copy brought more than the source held");
}

void test_a_copy_never_replaces_what_is_already_there() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"source.txt";
  const std::filesystem::path occupied = temporary.root() / L"occupied.txt";
  write_text_file(source, "new");
  write_text_file(occupied, "original");

  require_win32_error(
      [&] { copy_volume_entry(source.wstring(), occupied.wstring()); },
      ERROR_FILE_EXISTS, "a copy replaced an existing file");
  require(read_text_file(occupied) == "original",
          "the refused copy overwrote the destination");

  // 🔴 The one the rollback must not get wrong. A directory already standing
  // at the destination belongs to the user, so the copy has to fail without
  // merging into it and without the rollback taking it away on the way out.
  const std::filesystem::path tree = temporary.root() / L"tree";
  make_directory(tree);
  write_text_file(tree / L"inside.txt", "inside");
  const std::filesystem::path standing = temporary.root() / L"standing";
  make_directory(standing);
  write_text_file(standing / L"kept.txt", "kept");

  require_win32_error(
      [&] { copy_volume_entry(tree.wstring(), standing.wstring()); },
      ERROR_ALREADY_EXISTS, "a copy went ahead onto an existing directory");
  require(read_text_file(standing / L"kept.txt") == "kept",
          "the refused copy took away what the destination already held");
  require(!path_exists(standing / L"inside.txt"),
          "the refused copy merged into the existing directory");
}

// A cancel while the bytes of one file are moving. `CopyFileEx` is told to
// cancel rather than stop, which is what makes it take the part-written file
// away itself.
void test_a_cancelled_file_copy_leaves_no_fragment() {
  constexpr std::uintmax_t kBytes = 8u * 1024u * 1024u;
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"large.bin";
  const std::filesystem::path destination = temporary.root() / L"large-copy.bin";
  make_file(source, static_cast<std::size_t>(kBytes));

  // Quiet for the one question asked before anything opens, then stop for
  // every question the file system asks while it writes.
  int asked = 0;
  require_win32_error(
      [&] {
        copy_volume_entry(source.wstring(), destination.wstring(),
                          [&asked] { return ++asked > 1; });
      },
      ERROR_CANCELLED, "a cancelled copy did not report being cancelled");

  require(!path_exists(destination), "a cancelled copy left a fragment behind");
  require(std::filesystem::file_size(source) == kBytes,
          "a cancelled copy changed the source");
}

void test_a_cancelled_directory_copy_leaves_nothing_behind() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"tree";
  make_directory(source / L"inner");
  write_text_file(source / L"one.txt", "one");
  write_text_file(source / L"inner" / L"two.txt", "two");
  write_text_file(source / L"inner" / L"three.txt", "three");
  const std::filesystem::path destination = temporary.root() / L"tree-copy";

  // Quiet through the survey, which runs before the destination exists, and
  // then stops the copy as soon as it has actually written something.
  require_win32_error(
      [&] {
        copy_volume_entry(
            source.wstring(), destination.wstring(),
            [&destination] { return count_files_below(destination) > 0; });
      },
      ERROR_CANCELLED,
      "a cancelled directory copy did not report being cancelled");

  require(!path_exists(destination),
          "a cancelled copy left part of the tree behind");
  require(count_files_below(source) == 3,
          "a cancelled copy changed the source");
}

// Failing part-way through a tree is the case the rollback exists for, and an
// unreadable file produces it without needing a full disk or a pulled drive.
void test_a_failed_copy_leaves_nothing_behind() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"tree";
  make_directory(source / L"inner");
  write_text_file(source / L"one.txt", "one");
  write_text_file(source / L"inner" / L"locked.txt", "locked");
  write_text_file(source / L"inner" / L"two.txt", "two");
  const std::filesystem::path destination = temporary.root() / L"tree-copy";

  const ExclusiveFile held(source / L"inner" / L"locked.txt");
  require(held.held(), "the fixture could not hold a file open");

  require_win32_error(
      [&] { copy_volume_entry(source.wstring(), destination.wstring()); },
      ERROR_SHARING_VIOLATION,
      "a copy of an unreadable file did not report what stopped it");
  require(!path_exists(destination),
          "a failed copy left part of the tree behind");
}

// 🔴 Following a link reads what is off the volume; recreating it writes a way
// out of the container into the container. Neither is a copy.
void test_a_link_in_the_source_is_refused_before_anything_is_copied() {
  const TemporaryDirectory temporary;
  const std::filesystem::path outside = temporary.root() / L"outside";
  make_directory(outside);
  write_text_file(outside / L"precious.txt", "precious");

  const std::filesystem::path source = temporary.root() / L"tree";
  make_directory(source);
  write_text_file(source / L"ordinary.txt", "ordinary");
  if (!try_make_junction(source / L"link", outside)) {
    std::cout << "  (skipped: this machine would not create a junction)\n";
    return;
  }

  const std::filesystem::path destination = temporary.root() / L"tree-copy";
  require_throws(
      [&] { copy_volume_entry(source.wstring(), destination.wstring()); },
      "a tree holding a link was copied");
  require(!path_exists(destination),
          "the refused copy still built part of the destination");
  require(read_text_file(outside / L"precious.txt") == "precious",
          "the refused copy disturbed what the link pointed at");

  require_throws(
      [&] {
        copy_volume_entry((source / L"link").wstring(),
                          (temporary.root() / L"link-copy").wstring());
      },
      "a link was copied as an entry of its own");
  require(!path_exists(temporary.root() / L"link-copy"),
          "copying a link left something behind");
}

void test_a_copy_that_would_not_fit_is_refused_before_it_starts() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"huge.bin";
  // Eight terabytes claimed rather than occupied: past any free space this
  // runs against, and still inside what NTFS will let one file say it is. The
  // file itself costs nothing to make.
  if (!try_make_sparse_file(source, std::uint64_t{1} << 43)) {
    std::cout << "  (skipped: this file system has no sparse files)\n";
    return;
  }

  const std::filesystem::path destination = temporary.root() / L"huge-copy.bin";
  require_win32_error(
      [&] { copy_volume_entry(source.wstring(), destination.wstring()); },
      ERROR_DISK_FULL, "a copy with nowhere to put the bytes went ahead");
  require(!path_exists(destination),
          "the refused copy created the destination anyway");
}

void test_an_entry_cannot_be_copied_inside_itself() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"tree";
  make_directory(source);
  write_text_file(source / L"one.txt", "one");

  require_throws(
      [&] {
        copy_volume_entry(source.wstring(), (source / L"inner").wstring());
      },
      "a directory was copied into itself");
  require_throws([&] { copy_volume_entry(source.wstring(), source.wstring()); },
                 "a directory was copied onto itself");

  // `X:\treehouse` is not inside `X:\tree`, and must still be a place to copy.
  const std::filesystem::path sibling = temporary.root() / L"treehouse";
  copy_volume_entry(source.wstring(), sibling.wstring());
  require(read_text_file(sibling / L"one.txt") == "one",
          "a copy to a name that merely starts the same was refused");
}

void test_a_move_by_copying_takes_the_source_away() {
  const TemporaryDirectory temporary;
  const std::filesystem::path file = temporary.root() / L"one.txt";
  const std::filesystem::path moved = temporary.root() / L"moved.txt";
  write_text_file(file, "carried");

  move_volume_entry_by_copying(file.wstring(), moved.wstring());
  require(!path_exists(file), "the move by copying left the source behind");
  require(read_text_file(moved) == "carried",
          "the move by copying did not carry the contents");

  const std::filesystem::path tree = temporary.root() / L"tree";
  make_directory(tree / L"inner");
  write_text_file(tree / L"inner" / L"deep.txt", "deep");
  const std::filesystem::path elsewhere = temporary.root() / L"elsewhere";

  move_volume_entry_by_copying(tree.wstring(), elsewhere.wstring());
  require(!path_exists(tree), "the move by copying left the source tree behind");
  require(read_text_file(elsewhere / L"inner" / L"deep.txt") == "deep",
          "the move by copying did not carry the tree");
}

// The source goes only once the copy is whole, so a transfer that does not
// finish costs nothing.
void test_a_move_by_copying_that_does_not_finish_leaves_the_source_alone() {
  const TemporaryDirectory temporary;
  const std::filesystem::path source = temporary.root() / L"tree";
  make_directory(source);
  write_text_file(source / L"one.txt", "one");
  write_text_file(source / L"two.txt", "two");

  const std::filesystem::path occupied = temporary.root() / L"occupied";
  make_directory(occupied);
  write_text_file(occupied / L"kept.txt", "kept");

  require_win32_error(
      [&] {
        move_volume_entry_by_copying(source.wstring(), occupied.wstring());
      },
      ERROR_ALREADY_EXISTS,
      "a move by copying went ahead onto an existing name");
  require(count_files_below(source) == 2,
          "the refused move by copying took the source away");
  require(read_text_file(occupied / L"kept.txt") == "kept",
          "the refused move by copying disturbed the destination");

  const std::filesystem::path destination = temporary.root() / L"tree-copy";
  require_win32_error(
      [&] {
        move_volume_entry_by_copying(
            source.wstring(), destination.wstring(),
            [&destination] { return count_files_below(destination) > 0; });
      },
      ERROR_CANCELLED,
      "a cancelled move by copying did not report being cancelled");
  require(count_files_below(source) == 2,
          "a cancelled move by copying took the source away");
  require(!path_exists(destination),
          "a cancelled move by copying left the half-made destination");
}

void test_a_copy_through_an_escaping_parent_is_refused() {
  const TemporaryDirectory temporary;
  write_text_file(temporary.root() / L"source.txt", "here");
  const SubstitutedDrive substituted(temporary.path());
  if (!substituted.available()) {
    std::cout << "  (skipped: no free drive letter on this machine)\n";
    return;
  }

  require_throws(
      [&] {
        copy_volume_entry((temporary.root() / L"source.txt").wstring(),
                          substituted.child(L"destination.txt"));
      },
      "a copy whose destination resolves onto another volume went ahead");
  require(!path_exists(temporary.root() / L"destination.txt"),
          "the refused copy wrote the file anyway");

  require_throws(
      [&] {
        copy_volume_entry(substituted.child(L"source.txt"),
                          (temporary.root() / L"other.txt").wstring());
      },
      "a copy whose source resolves onto another volume went ahead");
  require(!path_exists(temporary.root() / L"other.txt"),
          "the refused copy wrote the destination anyway");
}

}  // namespace

int main() {
  try {
    test_numbered_exports_preserve_existing_entries();
    test_a_file_is_deleted();
    test_a_read_only_file_is_deleted();
    test_a_directory_goes_with_everything_inside_it();
    test_a_recursive_delete_unlinks_a_junction_and_spares_its_target();
    test_deleting_a_junction_leaves_its_target();
    test_an_entry_reached_through_an_escaping_parent_is_refused();
    test_renaming_and_moving_are_the_same_operation();
    test_a_move_never_replaces_what_is_already_there();
    test_a_directory_cannot_move_inside_itself();
    test_a_move_across_volumes_is_refused_here();
    test_a_directory_is_created_but_never_a_chain_of_them();
    test_a_path_that_is_not_drive_absolute_is_refused();
    test_a_missing_entry_is_reported_rather_than_ignored();
    test_a_file_is_copied_and_the_source_stays();
    test_a_directory_is_copied_with_everything_inside_it();
    test_a_copy_never_replaces_what_is_already_there();
    test_a_cancelled_file_copy_leaves_no_fragment();
    test_a_cancelled_directory_copy_leaves_nothing_behind();
    test_a_failed_copy_leaves_nothing_behind();
    test_a_link_in_the_source_is_refused_before_anything_is_copied();
    test_a_copy_that_would_not_fit_is_refused_before_it_starts();
    test_an_entry_cannot_be_copied_inside_itself();
    test_a_move_by_copying_takes_the_source_away();
    test_a_move_by_copying_that_does_not_finish_leaves_the_source_alone();
    test_a_copy_through_an_escaping_parent_is_refused();
  } catch (const std::exception& error) {
    std::cerr << "FAILED: unexpected exception: " << error.what() << '\n';
    return 1;
  }
  if (g_failures != 0) {
    std::cerr << g_failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "volume file operations tests passed\n";
  return 0;
}
