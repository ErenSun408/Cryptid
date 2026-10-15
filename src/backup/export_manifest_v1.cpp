#include "usb_vault/backup/export_manifest_v1.h"

#include "usb_vault/volume/volume_entry_path.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace usb_vault::backup {
namespace {

constexpr std::size_t kEntryFixedBytes = 1 + 2 + 8 + 8;
constexpr std::size_t kReadBlock = kExportPackageV1ChunkSize;

[[noreturn]] void reject() { throw ExportPackageRejected(); }

// Every device name, checked here on top of the shared resolver. A restored
// entry is created by name, and any of these would open a device instead.
constexpr std::array<std::wstring_view, 34> kReservedStems{
    L"CON",  L"PRN",  L"AUX",  L"NUL",  L"CONIN$", L"CONOUT$",
    L"COM0", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7",
    L"COM8", L"COM9", L"LPT0", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
    L"LPT6", L"LPT7", L"LPT8", L"LPT9", L"COM\u00B9", L"COM\u00B2",
    L"COM\u00B3", L"LPT\u00B9", L"LPT\u00B2", L"LPT\u00B3", L"CLOCK$",
    L"$MFT"};

[[nodiscard]] std::wstring upper(const std::wstring_view text) {
  if (text.empty()) return {};
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    reject();
  }
  std::wstring result(text.size(), L'\0');
  const int written = ::LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.data(),
      static_cast<int>(text.size()), result.data(),
      static_cast<int>(result.size()), nullptr, nullptr, 0);
  if (written != static_cast<int>(result.size())) reject();
  return result;
}

// Windows trims spaces and dots from the stem before it compares it with a
// device name, so `NUL .txt` and `COM1  .log` open devices too.
void validate_components(const std::wstring_view path) {
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t end = std::min(path.find(L'\\', start), path.size());
    const std::wstring_view component = path.substr(start, end - start);
    std::wstring_view stem = component.substr(0, std::min(component.find(L'.'), component.size()));
    while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.remove_suffix(1);
    const std::wstring folded = upper(stem);
    if (std::ranges::find(kReservedStems, std::wstring_view(folded)) != kReservedStems.end()) {
      reject();
    }
    start = end + 1;
  }
}

void validate_path(const std::wstring_view path, const bool top_level) {
  if (path.empty() || path.find(L'/') != std::wstring_view::npos ||
      path.front() == L'\\' || path.back() == L'\\') {
    reject();
  }
  const bool nested = path.find(L'\\') != std::wstring_view::npos;
  if (nested == top_level) reject();
  try {
    static_cast<void>(volume::resolve_volume_entry_path(L'A', path));
  } catch (const std::invalid_argument&) {
    reject();
  }
  validate_components(path);
}

class Cursor final {
 public:
  explicit Cursor(const std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] std::span<const std::byte> take(const std::size_t size) {
    if (size > bytes_.size() - offset_) reject();
    const auto result = bytes_.subspan(offset_, size);
    offset_ += size;
    return result;
  }
  [[nodiscard]] std::uint64_t number(const std::size_t size) {
    const auto bytes = take(size);
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) {
      value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
  }
  [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }

 private:
  std::span<const std::byte> bytes_;
  std::size_t offset_{};
};

void throw_cancelled() {
  throw std::system_error(ERROR_CANCELLED, std::system_category(),
                          "export package verification was cancelled");
}

}  // namespace

void validate_export_manifest_v1(const ExportManifest& manifest) {
  if (manifest.entries.empty() ||
      manifest.entries.size() > kExportManifestV1MaximumEntries) {
    reject();
  }
  // Each path, uppercased, to whether it is a directory.
  std::unordered_map<std::wstring, bool> seen;
  seen.reserve(manifest.entries.size());
  std::uint64_t total = 0;
  for (std::size_t index = 0; index < manifest.entries.size(); ++index) {
    const ExportManifestEntry& entry = manifest.entries[index];
    const bool directory = entry.kind == ExportEntryKind::directory;
    if (!directory && entry.kind != ExportEntryKind::file) reject();
    validate_path(entry.relative_path, index == 0);
    if (directory && entry.size != 0) reject();
    if (index > 0) {
      const std::size_t separator = entry.relative_path.rfind(L'\\');
      const auto parent =
          seen.find(upper(std::wstring_view(entry.relative_path).substr(0, separator)));
      if (parent == seen.end() || !parent->second) reject();
    }
    if (!seen.emplace(upper(entry.relative_path), directory).second) reject();
    if (entry.size > std::numeric_limits<std::uint64_t>::max() - total) reject();
    total += entry.size;
  }
  if (total != manifest.data_bytes) reject();
}

WipedBytes encode_export_manifest_v1(const ExportManifest& manifest) {
  try {
    validate_export_manifest_v1(manifest);
  } catch (const ExportPackageRejected&) {
    throw std::invalid_argument("the export manifest breaks a package rule");
  }
  std::uint64_t size = 0;
  for (const ExportManifestEntry& entry : manifest.entries) {
    size += kEntryFixedBytes + 2 * entry.relative_path.size();
  }
  if (size > kExportManifestV1MaximumBytes) {
    throw std::invalid_argument("the export manifest is too large");
  }
  WipedBytes out(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  const auto put = [&](const std::uint64_t value, const std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
      out.bytes()[offset++] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
    }
  };
  for (const ExportManifestEntry& entry : manifest.entries) {
    put(static_cast<std::uint8_t>(entry.kind), 1);
    put(entry.relative_path.size(), 2);
    for (const wchar_t unit : entry.relative_path) put(static_cast<std::uint16_t>(unit), 2);
    put(entry.size, 8);
    put(entry.last_write_time, 8);
  }
  return out;
}

ExportManifest decode_export_manifest_v1(const std::span<const std::byte> bytes,
                                         const ExportPackageV1Header& header) {
  if (header.manifest_bytes != bytes.size() ||
      header.manifest_bytes > kExportManifestV1MaximumBytes ||
      header.entry_count == 0 ||
      header.entry_count > kExportManifestV1MaximumEntries ||
      header.entry_count > bytes.size() / kEntryFixedBytes) {
    reject();
  }
  ExportManifest manifest;
  manifest.data_bytes = header.data_bytes;
  manifest.entries.reserve(static_cast<std::size_t>(header.entry_count));
  Cursor cursor(bytes);
  for (std::uint64_t i = 0; i < header.entry_count; ++i) {
    ExportManifestEntry entry;
    entry.kind = static_cast<ExportEntryKind>(cursor.number(1));
    const auto length = static_cast<std::size_t>(cursor.number(2));
    if (length == 0 || length > volume::kVolumePathMaximumLength) reject();
    entry.relative_path.resize(length);
    for (wchar_t& unit : entry.relative_path) {
      unit = static_cast<wchar_t>(cursor.number(2));
    }
    entry.size = cursor.number(8);
    entry.last_write_time = cursor.number(8);
    manifest.entries.push_back(std::move(entry));
  }
  if (!cursor.done()) reject();
  validate_export_manifest_v1(manifest);
  return manifest;
}

ExportPackageV1Header export_package_v1_header(const ExportManifest& manifest,
                                               const std::uint64_t manifest_bytes) {
  return ExportPackageV1Header{.manifest_bytes = manifest_bytes,
                               .data_bytes = manifest.data_bytes,
                               .entry_count = manifest.entries.size()};
}

ExportManifest read_export_manifest_v1(ExportPackageReaderV1& reader) {
  const ExportPackageV1Header& header = reader.header();
  // Checked before the allocation: the header is authenticated, but whoever
  // knows the password chose it.
  if (header.manifest_bytes > kExportManifestV1MaximumBytes ||
      header.entry_count > kExportManifestV1MaximumEntries ||
      reader.remaining() != header.manifest_bytes + header.data_bytes) {
    reject();
  }
  WipedBytes bytes(static_cast<std::size_t>(header.manifest_bytes));
  reader.read(bytes.bytes());
  return decode_export_manifest_v1(bytes.bytes(), header);
}

ExportManifest verify_export_package_v1(ExportPackageReaderV1& reader,
                                        const std::function<bool()>& cancelled,
                                        const ExportProgress& progress) {
  if (cancelled && cancelled()) throw_cancelled();
  ExportManifest manifest = read_export_manifest_v1(reader);
  const std::uint64_t total = manifest.data_bytes;
  WipedBytes block(static_cast<std::size_t>(std::min<std::uint64_t>(kReadBlock, total)));
  std::uint64_t done = 0;
  if (progress) progress(done, total);
  while (done < total) {
    if (cancelled && cancelled()) throw_cancelled();
    const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(kReadBlock, total - done));
    reader.read(block.bytes().first(size));
    block.wipe();
    done += size;
    if (progress) progress(done, total);
  }
  return manifest;
}

}  // namespace usb_vault::backup
