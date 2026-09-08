#include "usb_vault/volume/volume_file_operations.h"

#include "volume_test_support.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using usb_vault::volume::create_volume_directory;
using usb_vault::volume::delete_volume_entry;
using usb_vault::volume::move_volume_entry;
using usb_vault_test::path_exists;
using usb_vault_test::make_directory;
using usb_vault_test::make_file;
using usb_vault_test::SubstitutedDrive;
using usb_vault_test::TemporaryDirectory;
using usb_vault_test::try_make_junction;

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

}  // namespace

int main() {
  try {
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
