#include "usb_vault/application/container_index_view_v1.h"

#include "usb_vault/application/container_index_v1.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::application {
namespace {

constexpr std::string_view kSharedIndexKeyDomain =
    "usb-vault:container-index-shared:v1";
constexpr std::size_t kRootSecretSize = 32;

void require_success(const NTSTATUS status, const char *const operation) {
  if (!BCRYPT_SUCCESS(status)) {
    throw std::system_error(static_cast<int>(status), std::system_category(),
                            operation);
  }
}

template <typename Handle, auto Close> class UniqueHandle final {
public:
  UniqueHandle() noexcept = default;
  ~UniqueHandle() noexcept { reset(); }

  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;

  UniqueHandle(UniqueHandle &&other) noexcept
      : handle_(std::exchange(other.handle_, nullptr)) {}
  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other) {
      reset();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }

  [[nodiscard]] Handle get() const noexcept { return handle_; }
  [[nodiscard]] Handle *put() noexcept {
    reset();
    return &handle_;
  }

private:
  void reset() noexcept {
    if (handle_ != nullptr) {
      static_cast<void>(Close(handle_));
      handle_ = nullptr;
    }
  }

  Handle handle_{nullptr};
};

NTSTATUS close_algorithm(const BCRYPT_ALG_HANDLE handle) noexcept {
  return ::BCryptCloseAlgorithmProvider(handle, 0);
}

using AlgorithmHandle = UniqueHandle<BCRYPT_ALG_HANDLE, close_algorithm>;
using HashHandle = UniqueHandle<BCRYPT_HASH_HANDLE, ::BCryptDestroyHash>;

[[nodiscard]] ULONG checked_ulong(const std::size_t size) {
  if (size > std::numeric_limits<ULONG>::max()) {
    throw std::length_error("CNG input is too large");
  }
  return static_cast<ULONG>(size);
}

[[nodiscard]] int checked_int(const std::size_t size) {
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("a path is too long to compare");
  }
  return static_cast<int>(size);
}

[[nodiscard]] PUCHAR
mutable_bytes(const std::span<const std::byte> bytes) noexcept {
  const auto *const data = reinterpret_cast<const UCHAR *>(bytes.data());
  return const_cast<PUCHAR>(data);
}

[[nodiscard]] ULONG object_length(const BCRYPT_ALG_HANDLE algorithm) {
  ULONG value = 0;
  ULONG written = 0;
  require_success(::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                      reinterpret_cast<PUCHAR>(&value),
                                      sizeof(value), &written, 0),
                  "read CNG object length");
  if (written != sizeof(value) || value == 0) {
    throw std::runtime_error("invalid CNG object length");
  }
  return value;
}

[[nodiscard]] std::uint32_t random_u32() {
  std::uint32_t value = 0;
  require_success(
      ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value),
                        static_cast<ULONG>(sizeof(value)),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG),
      "BCryptGenRandom");
  return value;
}

// Rejection sampling rather than a plain remainder. A remainder would make the
// low slots marginally likelier, and the point of the choice is that it says
// nothing at all.
[[nodiscard]] std::size_t random_below(const std::size_t bound) {
  const auto limit = static_cast<std::uint32_t>(bound);
  const std::uint32_t threshold = (0U - limit) % limit;
  std::uint32_t draw = 0;
  do {
    draw = random_u32();
  } while (draw < threshold);
  return draw % limit;
}

// Windows matches file names without regard to case, so two spellings that
// differ only in case name one container and belong in one slot. Spellings
// that reach the same file another way -- a short name, a junction -- are not
// recognized here and would be remembered twice.
[[nodiscard]] bool same_path(const std::wstring_view left,
                             const std::wstring_view right) {
  if (left.empty() || right.empty()) {
    return left.empty() && right.empty();
  }
  return ::CompareStringOrdinal(left.data(), checked_int(left.size()),
                                right.data(), checked_int(right.size()),
                                TRUE) == CSTR_EQUAL;
}

struct LocatedEntry {
  std::size_t slot;
  ContainerIndexEntry entry;
};

std::vector<LocatedEntry> ordered_entries(
    const std::span<const std::byte> key,
    const std::span<const std::byte> file) {
  std::vector<LocatedEntry> entries;
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    auto opened = try_open_container_index_entry(
        key, container_index_v1_slot(file, index));
    if (opened) entries.push_back({index, std::move(*opened)});
  }
  std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    return a.entry.order.value_or(kContainerIndexV1SlotCount) <
           b.entry.order.value_or(kContainerIndexV1SlotCount);
  });
  return entries;
}

} // namespace

security::SecureBuffer derive_shared_container_index_key_v1(
    const std::span<const std::byte> root_secret) {
  if (root_secret.size() != kRootSecretSize) {
    throw std::invalid_argument("the card root secret has an invalid size");
  }

  // An HMAC fork off the root, the same shape card_record_v1 uses to reach its
  // envelope keys. No slow derivation belongs here: the secret being forked is
  // 32 random bytes, so there is no low-entropy guess for anyone to grind, and
  // charging a second Argon2id would only make logging in slower.
  AlgorithmHandle algorithm;
  require_success(
      ::BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM,
                                    nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
      "open SHA-256 HMAC provider");

  security::SecureBuffer hash_object(object_length(algorithm.get()));
  HashHandle hash;
  require_success(
      ::BCryptCreateHash(algorithm.get(), hash.put(),
                         reinterpret_cast<PUCHAR>(hash_object.data()),
                         checked_ulong(hash_object.size()),
                         mutable_bytes(root_secret),
                         checked_ulong(root_secret.size()), 0),
      "create container index HMAC");
  require_success(
      ::BCryptHashData(hash.get(),
                       mutable_bytes(std::as_bytes(
                           std::span(kSharedIndexKeyDomain))),
                       checked_ulong(kSharedIndexKeyDomain.size()), 0),
      "hash container index key domain");

  security::SecureBuffer key(kContainerIndexV1KeySize);
  require_success(::BCryptFinishHash(hash.get(),
                                     reinterpret_cast<PUCHAR>(key.data()),
                                     checked_ulong(key.size()), 0),
                  "finish container index HMAC");
  return key;
}

std::vector<std::wstring>
list_container_index_paths_v1(const std::span<const std::byte> key,
                              const std::span<const std::byte> file) {
  std::vector<std::wstring> paths;
  for (auto& located : ordered_entries(key, file))
    if (!located.entry.partition) paths.push_back(std::move(located.entry.path));
  return paths;
}

void reorder_container_index_paths_v1(
    const std::span<const std::byte> key, const std::span<std::byte> file,
    const std::span<const std::wstring> paths) {
  if (paths.size() > kContainerIndexV1SlotCount)
    throw std::invalid_argument("too many container paths");
  auto entries = ordered_entries(key, file);
  std::erase_if(entries, [](const auto& item) { return item.entry.partition.has_value(); });
  std::vector<std::size_t> order;
  std::vector<bool> selected(entries.size(), false);
  for (const auto& path : paths) {
    const auto found = std::find_if(entries.begin(), entries.end(),
        [&](const auto& item) { return same_path(item.entry.path, path); });
    if (found == entries.end())
      throw std::invalid_argument("unknown container path");
    const auto index = static_cast<std::size_t>(found - entries.begin());
    if (selected[index]) throw std::invalid_argument("duplicate container path");
    selected[index] = true;
    order.push_back(index);
  }
  for (std::size_t index = 0; index < entries.size(); ++index)
    if (!selected[index]) order.push_back(index);

  // Stage all encryption before touching the caller's buffer. No record is
  // relocated into an opaque slot, even when it belongs to another key.
  std::vector<std::byte> updated(file.begin(), file.end());
  for (std::size_t rank = 0; rank < order.size(); ++rank) {
    const auto& item = entries[order[rank]];
    const auto sealed = seal_container_index_entry_v1(
        key, item.entry.path, static_cast<std::uint32_t>(rank));
    std::ranges::copy(sealed.bytes(),
        container_index_v1_mutable_slot(updated, item.slot).begin());
  }
  std::ranges::copy(updated, file.begin());
}

ContainerIndexWriteV1
add_container_index_path_v1(const std::span<const std::byte> key,
                            const std::span<std::byte> file,
                            const std::wstring_view path) {
  const std::span<const std::byte> readable(file);
  std::vector<std::size_t> free_slots;
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    const auto opened = try_open_container_index_entry(
        key, container_index_v1_slot(readable, index));
    if (!opened.has_value()) {
      free_slots.push_back(index);
    } else if (!opened->partition && same_path(opened->path, path)) {
      return ContainerIndexWriteV1::already_present;
    }
  }
  if (free_slots.empty()) {
    return ContainerIndexWriteV1::full;
  }

  const std::size_t chosen = free_slots[random_below(free_slots.size())];
  const security::SecureBuffer sealed =
      seal_container_index_entry_v1(key, path);
  std::ranges::copy(sealed.bytes(),
                    container_index_v1_mutable_slot(file, chosen).begin());
  return ContainerIndexWriteV1::added;
}

ContainerIndexWriteV1 prepend_container_index_path_v1(
    const std::span<const std::byte> key, const std::span<std::byte> file,
    const std::wstring_view path) {
  std::vector<std::byte> updated(file.begin(), file.end());
  const auto result = add_container_index_path_v1(key, updated, path);
  if (result != ContainerIndexWriteV1::added) return result;
  const std::wstring first(path);
  reorder_container_index_paths_v1(key, updated,
                                   std::span<const std::wstring>(&first, 1));
  std::ranges::copy(updated, file.begin());
  return ContainerIndexWriteV1::added;
}

std::vector<ContainerIndexPartition> list_container_index_partitions(
    const std::span<const std::byte> key, const std::span<const std::byte> file) {
  std::vector<ContainerIndexPartition> result;
  for (const auto& located : ordered_entries(key, file))
    if (located.entry.partition) result.push_back(*located.entry.partition);
  return result;
}

ContainerIndexWriteV1 add_container_index_partition(
    const std::span<const std::byte> key, const std::span<std::byte> file,
    const ContainerIndexPartition& partition) {
  // Validate even if the file is full; do not mutate until all crypto succeeds.
  const auto sealed = seal_container_index_partition(key, partition);
  std::vector<std::size_t> free_slots;
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    const auto entry = try_open_container_index_entry(key, container_index_v1_slot(file, index));
    if (!entry) free_slots.push_back(index);
    else if (entry->partition == partition) return ContainerIndexWriteV1::already_present;
  }
  if (free_slots.empty()) return ContainerIndexWriteV1::full;
  std::ranges::copy(sealed.bytes(), container_index_v1_mutable_slot(
      file, free_slots[random_below(free_slots.size())]).begin());
  return ContainerIndexWriteV1::added;
}

bool remove_container_index_partition(
    const std::span<const std::byte> key, const std::span<std::byte> file,
    const ContainerIndexPartition& partition) {
  if (!valid_container_index_partition(partition))
    throw std::invalid_argument("invalid partition index identity");
  std::vector<std::byte> updated(file.begin(), file.end());
  bool removed = false;
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    const auto entry = try_open_container_index_entry(key, container_index_v1_slot(file, index));
    if (entry && entry->partition == partition) {
      fill_container_index_slot_v1(container_index_v1_mutable_slot(updated, index));
      removed = true;
    }
  }
  if (removed) std::ranges::copy(updated, file.begin());
  return removed;
}

bool remove_container_index_path_v1(const std::span<const std::byte> key,
                                    const std::span<std::byte> file,
                                    const std::wstring_view path) {
  const std::span<const std::byte> readable(file);
  bool removed = false;
  for (std::size_t index = 0; index < kContainerIndexV1SlotCount; ++index) {
    const std::optional<std::wstring> opened =
        try_open_container_index_entry_v1(
            key, container_index_v1_slot(readable, index));
    if (opened.has_value() && same_path(*opened, path)) {
      fill_container_index_slot_v1(
          container_index_v1_mutable_slot(file, index));
      removed = true;
    }
  }
  return removed;
}

} // namespace usb_vault::application
