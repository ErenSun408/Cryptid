#pragma once

#include "usb_vault/backup/export_package_v1.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace usb_vault::backup {

// Upper bounds on what a package may declare, checked before anything is
// allocated for it, so a package cannot make an import reserve memory it
// never justifies.
inline constexpr std::uint64_t kExportManifestV1MaximumBytes = 64U * 1024U * 1024U;
inline constexpr std::uint64_t kExportManifestV1MaximumEntries = 1'000'000;

enum class ExportEntryKind : std::uint8_t { directory = 1, file = 2 };

// One entry, by its path relative to the destination it will be restored
// under. The first entry is the exported file or folder itself and is the only
// one without a separator; the rest follow their parent directory.
struct ExportManifestEntry final {
  ExportEntryKind kind{ExportEntryKind::file};
  std::wstring relative_path;
  std::uint64_t size{};             // zero for a directory
  std::uint64_t last_write_time{};  // FILETIME, UTC

  friend bool operator==(const ExportManifestEntry&,
                         const ExportManifestEntry&) = default;
};

struct ExportManifest final {
  std::vector<ExportManifestEntry> entries;
  std::uint64_t data_bytes{};  // the sum of every file size

  [[nodiscard]] const std::wstring& top_level_name() const {
    return entries.front().relative_path;
  }
};

// Checks a manifest against every rule an import relies on, throwing
// ExportPackageRejected on the first violation:
// - one to kExportManifestV1MaximumEntries entries, the first one the only
//   top-level entry, and a top-level file standing alone;
// - every path made of components a volume can hold: no empty, `.` or `..`
//   component, no root, drive, colon, `/`, control or wildcard character, no
//   trailing space or dot, no device name even with spaces or dots after its
//   stem (`NUL .txt`), nothing past the length
//   limits of `resolve_volume_entry_path`;
// - each later entry under a directory listed before it, no two paths equal
//   ignoring case, directories of size zero, and file sizes adding up to
//   `data_bytes` without overflow.
void validate_export_manifest_v1(const ExportManifest& manifest);

// The manifest as it is stored in the encrypted stream, in a buffer wiped on
// release. Encoding validates first and throws std::invalid_argument for a
// manifest that would not pass or would exceed the size limit; the size is
// known before anything is allocated.
//
// The names in an ExportManifest itself live in ordinary strings and are not
// wiped: they are the same names the file manager lists, and only file
// contents and the encoded stream are treated as plaintext to clear.
[[nodiscard]] WipedBytes encode_export_manifest_v1(
    const ExportManifest& manifest);

// Parses and validates stored manifest bytes against what the stream header
// declared. Throws ExportPackageRejected for anything malformed.
[[nodiscard]] ExportManifest decode_export_manifest_v1(
    std::span<const std::byte> bytes, const ExportPackageV1Header& header);

// The stream header a manifest is written under.
[[nodiscard]] ExportPackageV1Header export_package_v1_header(
    const ExportManifest& manifest, std::uint64_t manifest_bytes);

// Reads and validates the manifest from a reader positioned right after the
// stream header, leaving it at the first data byte.
[[nodiscard]] ExportManifest read_export_manifest_v1(
    ExportPackageReaderV1& reader);

// Called with bytes done and bytes in total while data is read.
using ExportProgress = std::function<void(std::uint64_t, std::uint64_t)>;

// The read-only pass an import makes before it creates or writes anything:
// reads the manifest, then authenticates every remaining chunk and discards
// the plaintext, which is wiped as it goes. Throws ExportPackageRejected for a
// damaged package and std::system_error with ERROR_CANCELLED when `cancelled`
// returns true; it is asked before every chunk.
[[nodiscard]] ExportManifest verify_export_package_v1(
    ExportPackageReaderV1& reader, const std::function<bool()>& cancelled = {},
    const ExportProgress& progress = {});

}  // namespace usb_vault::backup
