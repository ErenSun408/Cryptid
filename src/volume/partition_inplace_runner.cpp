#include "volume/partition_inplace_runner.h"
#include "usb_vault/security/secure_buffer.h"
#include <stdexcept>

namespace usb_vault::volume::detail {
InplaceResult run_partition_inplace(InplaceBackend& backend) noexcept {
  struct Close final {
    InplaceBackend& backend;
    ~Close() { backend.close(); }
  } close{backend};
  bool revoked = false;
  bool pause_requested = false;
  try {
    auto poll = [&] {
      const auto value = backend.control();
      if (value == InplaceControl::revoked) revoked = true;
      if (value == InplaceControl::pause) pause_requested = true;
      if (value != InplaceControl::proceed && value != InplaceControl::pause &&
          value != InplaceControl::revoked) throw std::runtime_error("invalid conversion control");
      return revoked ? InplaceControl::revoked : (pause_requested ? InplaceControl::pause : value);
    };
    if (poll() == InplaceControl::revoked) return InplaceResult::revoked;
    auto range = backend.open_authenticated();
    auto chunk = next_partition_inplace_chunk(range.partition_bytes, range.start, range.length);
    // Keep the locked plaintext working set below the default process quota.
    // The planner gives a maximum chunk; consume its trailing 64 KiB first.
    security::SecureBuffer buffer(64 * 1024);
    auto checkpoint = [&] {
      // Flush ciphertext before publishing its range, then flush the header.
      backend.flush();
      backend.save_progress(range);
      backend.flush();
    };
    for (;;) {
      const auto control = poll();
      if (control != InplaceControl::proceed) {
        checkpoint();
        const auto after = poll();
        return revoked || after == InplaceControl::revoked ? InplaceResult::revoked : InplaceResult::paused;
      }
      if (!chunk) {
        checkpoint();
        const auto after = poll();
        if (after == InplaceControl::revoked) return InplaceResult::revoked;
        if (after == InplaceControl::pause) return InplaceResult::paused;
        backend.finalize();
        backend.flush();
        return poll() == InplaceControl::revoked ? InplaceResult::revoked : InplaceResult::completed;
      }
      if (chunk->bytes > buffer.size()) {
        const auto skipped = chunk->bytes - buffer.size();
        chunk->source_offset_bytes += skipped;
        chunk->destination_offset_bytes += skipped;
        chunk->encryption_unit += skipped / 512;
        chunk->bytes = static_cast<std::uint32_t>(buffer.size());
      }
      auto bytes = buffer.bytes().first(chunk->bytes);
      if (backend.read(chunk->source_offset_bytes, bytes) != bytes.size())
        throw std::runtime_error("incomplete conversion read");
      backend.encrypt(chunk->encryption_unit, bytes);
      // A normal pause lets this block finish. Authentication revocation does not.
      if (poll() == InplaceControl::revoked) {
        buffer.clear();
        checkpoint();
        return InplaceResult::revoked;
      }
      if (backend.write(chunk->destination_offset_bytes, bytes) != bytes.size())
        throw std::runtime_error("incomplete conversion write");
      buffer.clear();
      range.start -= chunk->bytes;
      range.length += chunk->bytes;
      chunk = next_partition_inplace_chunk(range.partition_bytes, range.start, range.length);
    }
  } catch (...) {
    return revoked ? InplaceResult::revoked : InplaceResult::failed;
  }
}
}  // namespace usb_vault::volume::detail
