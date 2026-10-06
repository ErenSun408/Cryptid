#pragma once
#include <string_view>
namespace usb_vault::application {
// Changes only Hidden and System, on the opened regular file; verifies readback.
bool container_file_hidden(std::wstring_view path);
void set_container_file_visibility(std::wstring_view path, bool hidden);
}
