#pragma once
#include <string_view>
namespace usb_vault::application {
// Changes only Hidden and System, on the opened regular file; verifies readback.
bool container_file_hidden(std::wstring_view path);
void set_container_file_visibility(std::wstring_view path, bool hidden);
// Creates a directory that must not exist yet and gives it Hidden and System,
// verified through a handle; a directory it cannot protect is removed again.
void create_protected_directory(std::wstring_view path);
// Removes the directory only if it is empty; anything else throws.
void remove_empty_directory(std::wstring_view path);
}
