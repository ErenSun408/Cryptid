#include "card/vikey/vikey_artifact.h"
#include "vikey_test_artifact.h"

#include "usb_vault/card/card_error.h"

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

namespace {

using usb_vault::card::CardError;
using usb_vault::card::VerifiedLocalArtifact;
using usb_vault::card::make_error_code;
using usb_vault::card::verify_local_artifact;
using usb_vault::card::vikey::Sha256Digest;
using usb_vault::card::vikey::VerifiedVikeyDll;
using usb_vault::card::vikey::verify_vikey_dll;
using usb_vault::card::vikey::test::TemporaryVikeyArtifact;
using usb_vault::card::vikey::test::kFixtureDigest;

static_assert(!std::is_copy_constructible_v<VerifiedVikeyDll>);
static_assert(!std::is_copy_assignable_v<VerifiedVikeyDll>);
static_assert(std::is_move_constructible_v<VerifiedVikeyDll>);
static_assert(!std::is_move_assignable_v<VerifiedVikeyDll>);
static_assert(!std::is_copy_constructible_v<VerifiedLocalArtifact>);
static_assert(!std::is_copy_assignable_v<VerifiedLocalArtifact>);
static_assert(std::is_move_constructible_v<VerifiedLocalArtifact>);
static_assert(!std::is_move_assignable_v<VerifiedLocalArtifact>);

void require(const bool condition, const char* const message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Operation>
void require_card_error(Operation&& operation, const CardError expected,
                        const char* const message) {
  try {
    std::forward<Operation>(operation)();
  } catch (const std::system_error& error) {
    require(error.code() == make_error_code(expected), message);
    return;
  }
  throw std::runtime_error(message);
}

void test_matching_digest_returns_locked_identity_token() {
  TemporaryVikeyArtifact artifact;
  {
    VerifiedVikeyDll verified = artifact.verified();
    require(verified.path() == artifact.path(),
            "verified ViKey path changed unexpectedly");
    require(!artifact.can_open_for_write(),
            "verified ViKey artifact remained writable");
    require(!artifact.can_open_for_delete(),
            "verified ViKey artifact remained deletable");
    require(!artifact.can_round_trip_rename_directory(),
            "verified ViKey deployment directory remained renameable");
    require(!artifact.can_round_trip_rename_ancestor(),
            "verified ViKey ancestor directory remained renameable");
  }
  require(artifact.can_open_for_write(),
          "ViKey write lock outlived the verification token");
  require(artifact.can_open_for_delete(),
          "ViKey delete lock outlived the verification token");
  require(artifact.can_round_trip_rename_directory(),
          "ViKey directory lock outlived the verification token");
  require(artifact.can_round_trip_rename_ancestor(),
          "ViKey ancestor lock outlived the verification token");
}

void test_digest_mismatch_fails_closed_and_releases_locks() {
  TemporaryVikeyArtifact artifact;
  Sha256Digest wrong_digest = kFixtureDigest;
  wrong_digest.front() ^= std::byte{0x01};
  require_card_error(
      [&] {
        static_cast<void>(verify_vikey_dll(artifact.path().parent_path(),
                                           wrong_digest));
      },
      CardError::backend_unavailable,
      "ViKey digest mismatch escaped the generic backend error");
  require(artifact.can_open_for_write(),
          "ViKey write lock leaked after digest mismatch");
  require(artifact.can_open_for_delete(),
          "ViKey delete lock leaked after digest mismatch");
}

void test_relative_path_is_rejected() {
  require_card_error(
      [&] {
        static_cast<void>(
            verify_vikey_dll(std::filesystem::path(L"relative"),
                             kFixtureDigest));
      },
      CardError::invalid_configuration,
      "relative ViKey path was not rejected");
}

void test_alternate_data_stream_path_is_rejected() {
  TemporaryVikeyArtifact artifact;
  std::filesystem::path alternate_stream = artifact.path();
  alternate_stream += L":payload";
  require_card_error(
      [&] {
        static_cast<void>(verify_local_artifact(
            alternate_stream, kFixtureDigest, 16U * 1024U * 1024U));
      },
      CardError::invalid_configuration,
      "alternate data stream artifact path was not rejected");
}

void test_empty_and_oversized_artifacts_are_rejected() {
  TemporaryVikeyArtifact artifact;
  artifact.resize(0);
  require_card_error(
      [&] {
        static_cast<void>(verify_vikey_dll(artifact.path().parent_path(),
                                           kFixtureDigest));
      },
      CardError::backend_unavailable,
      "empty ViKey artifact was not rejected");

  artifact.resize((16U * 1024U * 1024U) + 1U);
  require_card_error(
      [&] {
        static_cast<void>(verify_vikey_dll(artifact.path().parent_path(),
                                           kFixtureDigest));
      },
      CardError::backend_unavailable,
      "oversized ViKey artifact was not rejected");
}

}  // namespace

int main() {
  try {
    test_matching_digest_returns_locked_identity_token();
    test_digest_mismatch_fails_closed_and_releases_locks();
    test_relative_path_is_rejected();
    test_alternate_data_stream_path_is_rejected();
    test_empty_and_oversized_artifacts_are_rejected();
  } catch (const std::exception& error) {
    std::cerr << "vikey_artifact_tests: FAIL: " << error.what() << '\n';
    return 1;
  }

  std::cout << "vikey_artifact_tests: PASS\n";
  return 0;
}
