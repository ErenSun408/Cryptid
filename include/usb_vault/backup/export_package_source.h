#pragma once

#include "usb_vault/backup/export_manifest_v1.h"
#include "usb_vault/backup/export_package_v1.h"

#include <functional>
#include <string_view>

namespace usb_vault::backup {

// Walks the file or directory about to be exported and lists it as a
// manifest: the entry itself first, then its contents, every directory before
// what it holds. Only names, kinds, sizes and last-write times are taken.
//
// 🔴 A reparse point anywhere in the source -- a symbolic link, a junction --
// is refused with std::runtime_error before a byte is written. Following one
// would pack whatever it names, which may be off the volume entirely.
//
// Throws std::invalid_argument when a name breaks a package rule, so the
// export fails here rather than producing a package no import would accept,
// std::length_error past the entry limit, and std::system_error for what the
// file system refused, including ERROR_CANCELLED when asked to stop.
[[nodiscard]] ExportManifest collect_export_manifest_v1(
    std::wstring_view absolute_source,
    const std::function<bool()>& cancelled = {});

// Streams one package for a collected manifest: the encoded manifest, then
// each file's bytes in manifest order, sealed under `key` into `sink`.
//
// A file that is no longer the size the manifest recorded, has become a link
// or a directory, or cannot be read is a failure (std::runtime_error or
// std::system_error): what reached the sink is then not a package, and the
// caller discards it. Progress counts file bytes; `cancelled` is asked before
// every block and stops the export with ERROR_CANCELLED.
void write_export_package_v1(std::wstring_view absolute_source,
                             const ExportManifest& manifest,
                             ExportPackageKeyV1 key,
                             const ExportPackageWriterV1::Sink& sink,
                             const std::function<bool()>& cancelled = {},
                             const ExportProgress& progress = {});

}  // namespace usb_vault::backup
