#include "development_container_fixture.h"

#include "usb_vault/application/container_layout_v1.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace usb_vault::development {
namespace {

[[nodiscard]] security::SecureBuffer secure_text(
    const std::string_view value) {
  security::SecureBuffer result(value.size());
  std::ranges::transform(value, result.bytes().begin(), [](const char byte) {
    return static_cast<std::byte>(static_cast<unsigned char>(byte));
  });
  return result;
}

[[nodiscard]] security::SecureBuffer container_a_input() {
  security::SecureBuffer result(
      application::kContainerAPasswordInput.size());
  std::ranges::copy(application::kContainerAPasswordInput,
                    result.bytes().begin());
  return result;
}

}  // namespace

void require_regular_file(const std::filesystem::path& path,
                          const char* const description) {
  std::error_code error;
  const bool regular = std::filesystem::is_regular_file(path, error);
  if (error || !regular) {
    throw std::runtime_error(std::string(description) + " is unavailable");
  }
}

void require_absent(const std::filesystem::path& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    throw std::runtime_error("development fixture target already exists");
  }
  const DWORD error = ::GetLastError();
  if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "inspect development fixture target");
  }
}

FixtureCleanup::FixtureCleanup(std::filesystem::path stage_a,
                               std::filesystem::path stage_b,
                               std::filesystem::path final_a,
                               std::filesystem::path final_b)
    : stage_a_(std::move(stage_a)),
      stage_b_(std::move(stage_b)),
      final_a_(std::move(final_a)),
      final_b_(std::move(final_b)) {}

FixtureCleanup::~FixtureCleanup() noexcept {
  remove_if_owned(stage_a_, owns_stage_a_);
  remove_if_owned(stage_b_, owns_stage_b_);
  remove_if_owned(final_a_, owns_final_a_);
  remove_if_owned(final_b_, owns_final_b_);
}

void FixtureCleanup::own_stage_a() noexcept { owns_stage_a_ = true; }

void FixtureCleanup::own_stage_b() noexcept { owns_stage_b_ = true; }

void FixtureCleanup::publish_a() {
  move_owned(stage_a_, final_a_);
  owns_stage_a_ = false;
  owns_final_a_ = true;
}

void FixtureCleanup::publish_b() {
  move_owned(stage_b_, final_b_);
  owns_stage_b_ = false;
  owns_final_b_ = true;
}

void FixtureCleanup::release() noexcept {
  owns_final_a_ = false;
  owns_final_b_ = false;
}

void FixtureCleanup::remove_if_owned(const std::filesystem::path& path,
                                     const bool owned) noexcept {
  if (owned) static_cast<void>(::DeleteFileW(path.c_str()));
}

void FixtureCleanup::move_owned(const std::filesystem::path& source,
                                const std::filesystem::path& target) {
  if (::MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    throw std::system_error(static_cast<int>(::GetLastError()),
                            std::system_category(),
                            "publish development fixture container");
  }
}

void prepare_matching_development_containers(
    card::ICardStore& card_store,
    IDevelopmentContainerFixtureRuntime& runtime, FixtureCleanup& cleanup,
    const std::wstring_view container_a_path,
    const std::wstring_view container_b_path) {
  security::SecureBuffer card_secret = card_store.get_blob();
  if (card_secret.size() != security::kKdfV1CardSecretSize) {
    throw std::invalid_argument(
        "development card blob must contain exactly 32 bytes");
  }

  security::SecureBuffer a_input = container_a_input();
  security::SecureBuffer a_password = runtime.derive(
      a_input, card_secret, application::kContainerASalt);
  a_input.clear();

  security::SecureBuffer outer_input =
      secure_text(kP17DuressTestPassword);
  security::SecureBuffer outer_password = runtime.derive(
      outer_input, card_secret, application::kContainerBSalt);
  outer_input.clear();

  security::SecureBuffer hidden_input = secure_text(kP17RealTestPassword);
  security::SecureBuffer hidden_password = runtime.derive(
      hidden_input, card_secret, application::kContainerBSalt);
  hidden_input.clear();
  card_secret.clear();

  cleanup.own_stage_a();
  runtime.create_standard(container_a_path, kP17ContainerABytes,
                          a_password);
  a_password.clear();
  cleanup.own_stage_b();
  runtime.create_with_hidden(container_b_path, kP17ContainerBBytes,
                             outer_password, hidden_password);
  outer_password.clear();
  hidden_password.clear();
}

}  // namespace usb_vault::development
