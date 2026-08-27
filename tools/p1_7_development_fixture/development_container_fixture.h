#pragma once

#include "usb_vault/card/card_store.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/security/secure_buffer.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace usb_vault::development {

inline constexpr std::string_view kP17RealTestPassword =
    "p1.7-real-test";
inline constexpr std::string_view kP17DuressTestPassword =
    "p1.7-duress-test";
inline constexpr std::uint64_t kP17ContainerABytes =
    20ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kP17ContainerBBytes =
    100ULL * 1024ULL * 1024ULL;

class IDevelopmentContainerFixtureRuntime {
 public:
  virtual ~IDevelopmentContainerFixtureRuntime() noexcept = default;

  IDevelopmentContainerFixtureRuntime(
      const IDevelopmentContainerFixtureRuntime&) = delete;
  IDevelopmentContainerFixtureRuntime& operator=(
      const IDevelopmentContainerFixtureRuntime&) = delete;

  [[nodiscard]] virtual security::SecureBuffer derive(
      const security::SecureBuffer& password_input,
      const security::SecureBuffer& card_secret,
      const security::KdfV1Salt& salt) = 0;
  virtual void create_standard(
      std::wstring_view target_path, std::uint64_t size_bytes,
      const security::SecureBuffer& volume_password) = 0;
  virtual void create_with_hidden(
      std::wstring_view target_path, std::uint64_t outer_size_bytes,
      const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password) = 0;

 protected:
  IDevelopmentContainerFixtureRuntime() noexcept = default;
};

// Fails closed unless the path names an existing regular file.
void require_regular_file(const std::filesystem::path& path,
                          const char* description);

// Fails closed unless the path names nothing at all. An existing file or
// directory is never inspected, opened or overwritten, and a target that
// cannot be classified is treated as present.
void require_absent(const std::filesystem::path& path);

// Owns every file the fixture may have produced. A staging path is claimed
// before the creation call, so a container that was partially written before
// creation threw is still removed. Publication transfers ownership from the
// staging path to the final path, so a failure between the two publications
// rolls the first one back instead of leaving a half-published layout.
class FixtureCleanup final {
 public:
  FixtureCleanup(std::filesystem::path stage_a, std::filesystem::path stage_b,
                 std::filesystem::path final_a, std::filesystem::path final_b);
  ~FixtureCleanup() noexcept;

  FixtureCleanup(const FixtureCleanup&) = delete;
  FixtureCleanup& operator=(const FixtureCleanup&) = delete;

  void own_stage_a() noexcept;
  void own_stage_b() noexcept;
  void publish_a();
  void publish_b();
  void release() noexcept;

 private:
  static void remove_if_owned(const std::filesystem::path& path,
                              bool owned) noexcept;
  static void move_owned(const std::filesystem::path& source,
                         const std::filesystem::path& target);

  std::filesystem::path stage_a_;
  std::filesystem::path stage_b_;
  std::filesystem::path final_a_;
  std::filesystem::path final_b_;
  bool owns_stage_a_{false};
  bool owns_stage_b_{false};
  bool owns_final_a_{false};
  bool owns_final_b_{false};
};

// Reads the card exactly once and never writes it. All KDF inputs and outputs
// remain in SecureBuffer objects and are explicitly cleared after their final
// use. Each staging path is handed to the cleanup guard before the creation
// call it belongs to, so a container that fails halfway through creation is
// still removed. Publication is left to the caller.
void prepare_matching_development_containers(
    card::ICardStore& card_store,
    IDevelopmentContainerFixtureRuntime& runtime, FixtureCleanup& cleanup,
    std::wstring_view container_a_path,
    std::wstring_view container_b_path);

}  // namespace usb_vault::development
