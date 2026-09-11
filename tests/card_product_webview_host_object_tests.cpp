#include "usb_vault/ui/card_product_webview_host_object.h"

#include "usb_vault/card/card_error.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/security/offline_card_password_policy.h"

#include <Windows.h>
#include <OleAuto.h>
#include <wrl.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <thread>
#include <chrono>

namespace {

using Microsoft::WRL::ComPtr;
using usb_vault::application::CardAuthenticationSession;
using usb_vault::application::CardEntryRoute;
using usb_vault::application::ContainerAStatus;
using usb_vault::application::IAuthenticatedCardVolumeSession;
using usb_vault::application::ICardEntryActions;
using usb_vault::security::ConfirmedPassword;
using usb_vault::security::OfflineCardPasswordPolicy;
using usb_vault::security::SecureBuffer;
using usb_vault::ui::CardProductUiSessionController;
using usb_vault::ui::create_card_product_webview_host_object;

void require(const bool condition, const char* const message) {
  if (!condition) throw std::runtime_error(message);
}

[[nodiscard]] std::vector<std::byte> bytes(
    const std::string_view value) {
  std::vector<std::byte> result;
  result.reserve(value.size());
  for (const char character : value) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
}

[[nodiscard]] SecureBuffer root_secret() {
  SecureBuffer result(usb_vault::card::kCardRecordV1RootSecretSize);
  for (std::size_t index = 0; index < result.size(); ++index) {
    result.bytes()[index] = static_cast<std::byte>(index + 1);
  }
  return result;
}

[[nodiscard]] bool equals(const std::span<const std::byte> left,
                          const std::span<const std::byte> right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (left[index] != right[index]) return false;
  }
  return true;
}

class FakeVolumeSession final : public IAuthenticatedCardVolumeSession {
 public:
  usb_vault::application::CreationFeedback creation_feedback;
  std::atomic<bool> pause_creation{false};
  std::atomic<bool> creation_entered{false};
  void set_creation_feedback(usb_vault::application::CreationFeedback feedback) override {
    creation_feedback = std::move(feedback);
  }
  usb_vault::application::CreationCancellation creation_cancelled;
  void set_creation_cancellation(
      usb_vault::application::CreationCancellation cancelled) override {
    creation_cancelled = std::move(cancelled);
  }
  bool logging_enabled{true};
  bool operation_logging_enabled() override { return logging_enabled; }
  void set_operation_logging_enabled(bool enabled) override { logging_enabled = enabled; }
  explicit FakeVolumeSession(CardAuthenticationSession& authentication)
      : authentication_(authentication) {}

  void create_container_a(const std::uint64_t size_bytes) override {
    ++create_a_calls;
    created_a_size_bytes = size_bytes;
    if (reject_create_a) throw std::runtime_error("creation failed");
    container_a_exists = true;
    container_a_size_bytes = size_bytes;
    container_a_mounted = true;
    mounted_state = true;
  }

  std::uint32_t create_container(const std::wstring_view path,
                                 const std::uint64_t size_bytes) override {
    if (creation_feedback) { creation_feedback(1, 100, 0); creation_feedback(2, 100, 0); creation_feedback(3, 41, 0); }
    creation_entered.store(true);
    for (int i = 0; pause_creation.load() && i < 2000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++create_calls;
    created_path.assign(path);
    created_size_bytes = size_bytes;
    // What the real session does once the helper is done: a cancel still
    // pending here means the container is dropped rather than published.
    if (creation_cancelled && creation_cancelled()) {
      throw std::runtime_error("creation cancelled");
    }
    if (reject_create) throw std::runtime_error("creation failed");
    return adopt(path, size_bytes);
  }

  std::uint32_t create_container_b(
      const std::wstring_view path,
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const SecureBuffer&) override {
    ++create_b_calls;
    created_b_path.assign(path);
    created_outer_bytes = outer_size_bytes;
    created_hidden_bytes = hidden_size_bytes;
    return adopt(path, outer_size_bytes);
  }

  SecureBuffer copy_session_password() override {
    const std::vector<std::byte> retained = bytes("login-password");
    SecureBuffer copy(retained.size());
    for (std::size_t index = 0; index < retained.size(); ++index) {
      copy.bytes()[index] = retained[index];
    }
    return copy;
  }

  std::wstring created_path;
  std::wstring created_b_path;
  std::uint64_t created_outer_bytes{};
  std::uint64_t created_hidden_bytes{};
  int create_b_calls{};

  std::uint32_t mount_container(const std::wstring_view path) override {
    ++mount_calls;
    for (Adopted& entry : containers) {
      if (entry.path != path) continue;
      entry.mounted = true;
      mounted_state = true;
      return entry.id;
    }
    return adopt(path, 64ULL * 1024ULL * 1024ULL);
  }

  void mount_adopted_container(const std::uint32_t id) override {
    ++mount_adopted_calls;
    for (Adopted& entry : containers) {
      if (entry.id != id) continue;
      entry.mounted = true;
      mounted_state = true;
      return;
    }
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }

  int mount_adopted_calls{};
  void unmount_container(const std::uint32_t id) override {
    ++unmount_container_calls;
    for (Adopted& entry : containers) {
      if (entry.id != id) continue;
      entry.mounted = false;
      return;
    }
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }

  [[nodiscard]] bool remembering_container_paths() override {
    return remember_paths;
  }

  void set_remembering_container_paths(const bool remember) override {
    remember_paths = remember;
    ++remember_paths_writes;
  }

  std::uint32_t adopt_container(const std::wstring_view path) override {
    ++adopt_calls;
    adopted_path.assign(path);
    if (reject_adopt) throw std::runtime_error("adopt failed");
    for (const Adopted& entry : containers) {
      if (entry.path == path) return entry.id;
    }
    // An adopted container joins the list with no drive letter and no size:
    // nothing has opened it, so nothing is known about it yet.
    const std::size_t separator = path.find_last_of(L"\\/");
    const std::wstring_view name = separator == std::wstring_view::npos
                                       ? path
                                       : path.substr(separator + 1);
    const std::uint32_t id = next_id++;
    containers.push_back({.id = id,
                          .path = std::wstring(path),
                          .name = std::wstring(name),
                          .mounted = false});
    return id;
  }

  [[nodiscard]] bool mounting_imported_containers() override {
    return mount_imported;
  }

  void set_mounting_imported_containers(const bool mount) override {
    mount_imported = mount;
    ++mount_imported_writes;
  }

  bool remember_paths{true};
  int remember_paths_writes{};
  bool mount_imported{};
  int mount_imported_writes{};
  int adopt_calls{};
  bool reject_adopt{};
  std::wstring adopted_path;

  void forget_container(const std::uint32_t id) override {
    ++forget_container_calls;
    for (auto entry = containers.begin(); entry != containers.end(); ++entry) {
      if (entry->id != id) continue;
      if (entry->mounted) {
        throw std::logic_error(
            "a mounted container cannot be dropped from the session");
      }
      containers.erase(entry);
      return;
    }
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }

  // The file manager, recorded rather than performed. What this layer is
  // answerable for is which container and which strings it passed on, and
  // whether it refused before passing anything on at all.
  struct DirectoryCall final {
    std::uint32_t id{};
    std::wstring directory;
  };

  struct EntryCall final {
    std::uint32_t id{};
    std::wstring directory;
    std::wstring name;
    std::wstring new_name;
  };

  struct TransferCall final {
    std::uint32_t source_id{};
    std::wstring source_directory;
    std::wstring name;
    std::uint32_t destination_id{};
    std::wstring destination_directory;
  };

  [[nodiscard]] usb_vault::volume::VolumeDirectoryListing
  list_container_directory(
      const std::uint32_t id,
      const std::wstring_view relative_directory) override {
    listed.push_back({.id = id, .directory = std::wstring(relative_directory)});
    if (reject_listing) throw std::invalid_argument("no such directory");
    return listing;
  }

  void delete_container_entry(const std::uint32_t id,
                              const std::wstring_view relative_directory,
                              const std::wstring_view name) override {
    deleted.push_back({.id = id,
                       .directory = std::wstring(relative_directory),
                       .name = std::wstring(name)});
  }

  void rename_container_entry(const std::uint32_t id,
                              const std::wstring_view relative_directory,
                              const std::wstring_view name,
                              const std::wstring_view new_name) override {
    renamed.push_back({.id = id,
                       .directory = std::wstring(relative_directory),
                       .name = std::wstring(name),
                       .new_name = std::wstring(new_name)});
  }

  void create_container_directory(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view name) override {
    created_directories.push_back({.id = id,
                                   .directory =
                                       std::wstring(relative_directory),
                                   .name = std::wstring(name)});
  }

  void move_container_entry(
      const std::uint32_t source_id,
      const std::wstring_view source_relative_directory,
      const std::wstring_view name, const std::uint32_t destination_id,
      const std::wstring_view destination_relative_directory) override {
    moved.push_back(
        {.source_id = source_id,
         .source_directory = std::wstring(source_relative_directory),
         .name = std::wstring(name),
         .destination_id = destination_id,
         .destination_directory =
             std::wstring(destination_relative_directory)});
  }

  void copy_container_entry(
      const std::uint32_t source_id,
      const std::wstring_view source_relative_directory,
      const std::wstring_view name, const std::uint32_t destination_id,
      const std::wstring_view destination_relative_directory) override {
    copied.push_back(
        {.source_id = source_id,
         .source_directory = std::wstring(source_relative_directory),
         .name = std::wstring(name),
         .destination_id = destination_id,
         .destination_directory =
             std::wstring(destination_relative_directory)});
  }

  [[nodiscard]] std::size_t file_manager_calls() const noexcept {
    return listed.size() + deleted.size() + renamed.size() +
           created_directories.size() + moved.size() + copied.size();
  }

  usb_vault::volume::VolumeDirectoryListing listing;
  bool reject_listing{};
  std::vector<DirectoryCall> listed;
  std::vector<EntryCall> deleted;
  std::vector<EntryCall> renamed;
  std::vector<EntryCall> created_directories;
  std::vector<TransferCall> moved;
  std::vector<TransferCall> copied;

  struct ExportCall final {
    std::uint32_t id{};
    std::wstring directory;
    std::wstring name;
    std::wstring destination;
  };

  void export_container_entry(
      const std::uint32_t id, const std::wstring_view relative_directory,
      const std::wstring_view name,
      const std::wstring_view absolute_destination_directory) override {
    exported_entries.push_back(
        {.id = id,
         .directory = std::wstring(relative_directory),
         .name = std::wstring(name),
         .destination = std::wstring(absolute_destination_directory)});
  }

  void export_container_file(
      const std::uint32_t id,
      const std::wstring_view absolute_destination_directory) override {
    exported_files.push_back(
        {.id = id,
         .destination = std::wstring(absolute_destination_directory)});
  }

  std::vector<ExportCall> exported_entries;
  std::vector<ExportCall> exported_files;

  [[nodiscard]] std::vector<usb_vault::application::ContainerStatus>
  container_statuses() override {
    if (fail_container_statuses) {
      throw std::runtime_error("container status query failed");
    }
    std::vector<usb_vault::application::ContainerStatus> result;
    result.reserve(containers.size());
    for (const Adopted& entry : containers) {
      result.push_back({.id = entry.id,
                        .name = entry.name,
                        .mounted = entry.mounted,
                        .size_bytes = entry.mounted ? entry.size_bytes : 0,
                        .drive_letter =
                            entry.mounted ? entry.drive_letter : wchar_t{}});
    }
    return result;
  }

  struct Adopted final {
    std::uint32_t id{};
    std::wstring path;
    std::wstring name;
    std::uint64_t size_bytes{};
    wchar_t drive_letter{};
    bool mounted{};
  };

  std::uint32_t adopt(const std::wstring_view path,
                      const std::uint64_t size_bytes) {
    const std::size_t separator = path.find_last_of(L"\\/");
    const std::wstring_view name = separator == std::wstring_view::npos
                                       ? path
                                       : path.substr(separator + 1);
    const std::uint32_t id = next_id++;
    containers.push_back(
        {.id = id,
         .path = std::wstring(path),
         .name = std::wstring(name),
         .size_bytes = size_bytes,
         .drive_letter = static_cast<wchar_t>(
             L'D' + static_cast<wchar_t>(containers.size())),
         .mounted = true});
    mounted_state = true;
    return id;
  }

  std::vector<Adopted> containers;
  std::uint64_t created_size_bytes{};
  std::uint32_t next_id{1};
  bool reject_create{};
  bool fail_container_statuses{};
  bool fail_operation_history{};
  int create_calls{};
  int mount_calls{};
  int unmount_container_calls{};
  int forget_container_calls{};

  [[nodiscard]] ContainerAStatus container_a_status() override {
    return {.exists = container_a_exists,
            .mounted = container_a_mounted,
            .size_bytes = container_a_size_bytes,
            .drive_letter = container_a_mounted ? L'X' : wchar_t{}};
  }

  [[nodiscard]] std::vector<usb_vault::application::OperationEvent>
  operation_history() override {
    if (fail_operation_history) {
      throw std::runtime_error("operation history query failed");
    }
    return operation_events;
  }

  void release_session_secrets() noexcept override {}

  void clear_operation_history() noexcept override {
    ++clear_operation_history_calls;
    operation_events.clear();
  }

  void consume_authenticated_login(SecureBuffer root,
                                   SecureBuffer login_password_utf8) override {
    ++consume_calls;
    login_bytes.assign(login_password_utf8.bytes().begin(),
                       login_password_utf8.bytes().end());
    authentication_.establish(std::move(root));
    login_password_utf8.clear();
    mounted_state = mount_on_login;
  }

  void unmount_active() override {
    ++unmount_calls;
    if (reject_unmount) throw std::runtime_error("unmount failed");
    container_a_mounted = false;
    mounted_state = false;
  }

  [[nodiscard]] bool force_unmount_active() override {
    throw std::logic_error("forced unmount is outside the UI contract");
  }

  [[nodiscard]] bool mounted() const noexcept override {
    return mounted_state;
  }

  CardAuthenticationSession& authentication_;
  std::vector<std::byte> login_bytes;
  std::vector<usb_vault::application::OperationEvent> operation_events;
  bool mount_on_login{};
  bool mounted_state{};
  bool reject_unmount{};
  bool reject_create_a{};
  bool container_a_exists{};
  bool container_a_mounted{};
  std::uint64_t container_a_size_bytes{};
  std::uint64_t created_a_size_bytes{};
  int consume_calls{};
  int create_a_calls{};
  int unmount_calls{};
  int clear_operation_history_calls{};
};

class FakeEntry final : public ICardEntryActions {
 public:
  void initialize_and_authenticate(
      CardAuthenticationSession& authentication,
      ConfirmedPassword confirmed_daily_password) override {
    ++initialization_calls;
    SecureBuffer password = std::move(confirmed_daily_password).take();
    initialization_bytes.assign(password.bytes().begin(),
                                password.bytes().end());
    password.clear();
    route = CardEntryRoute::unavailable;
    authentication.establish(root_secret());
  }

  void authenticate_login(
      usb_vault::application::ICardLoginSuccessConsumer& success_consumer,
      SecureBuffer login_password_utf8) override {
    ++login_calls;
    if (reject_credential) {
      throw usb_vault::card::CardCredentialRejected();
    }
    route = CardEntryRoute::unavailable;
    success_consumer.consume_authenticated_login(
        root_secret(), std::move(login_password_utf8));
  }

  void set_second_card_password(SecureBuffer existing_password_utf8,
                                SecureBuffer second_password_utf8) override {
    ++second_password_calls;
    existing_password_bytes.assign(existing_password_utf8.bytes().begin(),
                                   existing_password_utf8.bytes().end());
    second_password_bytes.assign(second_password_utf8.bytes().begin(),
                                 second_password_utf8.bytes().end());
    existing_password_utf8.clear();
    second_password_utf8.clear();
  }

  void reset_card_record(
      const usb_vault::application::IVolatileCardAuthenticationSession&
          authentication) override {
    if (!authentication.authenticated()) {
      throw std::runtime_error("card reset was not authenticated");
    }
    ++reset_calls;
  }

  int reset_calls{};
  std::vector<std::byte> existing_password_bytes;
  std::vector<std::byte> second_password_bytes;
  int second_password_calls{};

  [[nodiscard]] std::optional<CardEntryRoute>
  current_route() const noexcept override {
    return route;
  }

  std::optional<CardEntryRoute> route;
  std::vector<std::byte> initialization_bytes;
  bool reject_credential{};
  int initialization_calls{};
  int login_calls{};
};

// Adoption stays native, and so does the browse button behind the form's
// save-location field: the page can only ask, so the fixture decides what
// comes back and an empty string stands for a cancelled dialog.
class FakeFileChooser final : public usb_vault::ui::IContainerFileChooser {
 public:
  [[nodiscard]] std::wstring choose_container_directory() override {
    ++directory_calls;
    return directory_path;
  }

  [[nodiscard]] std::wstring choose_existing_container() override {
    ++existing_calls;
    return existing_path;
  }

  std::wstring directory_path;
  std::wstring existing_path;
  int directory_calls{};
  int existing_calls{};
};

// The form names a location instead of picking one, and the boundary checks
// that the location exists, so the fixture needs a real directory.
class ScopedDirectory final {
 public:
  explicit ScopedDirectory(const wchar_t* const leaf)
      : path_(std::filesystem::temp_directory_path() /
              L"usb-vault-host-tests" / leaf) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    std::filesystem::create_directories(path_);
  }

  ~ScopedDirectory() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScopedDirectory(const ScopedDirectory&) = delete;
  ScopedDirectory& operator=(const ScopedDirectory&) = delete;

  [[nodiscard]] std::wstring wide() const { return path_.wstring(); }

  [[nodiscard]] std::wstring target(const std::wstring_view name) const {
    return wide() + L"\\" + std::wstring(name) + L".hc";
  }

 private:
  std::filesystem::path path_;
};

std::uint32_t g_fixture_serial = 0;

struct Fixture final {
  Fixture()
      : directory(std::to_wstring(++g_fixture_serial).c_str()),
        volume(authentication),
        controller(entry, authentication, volume, policy, chooser) {}

  FakeEntry entry;
  ScopedDirectory directory;
  CardAuthenticationSession authentication;
  FakeVolumeSession volume;
  OfflineCardPasswordPolicy policy;
  FakeFileChooser chooser;
  CardProductUiSessionController controller;
};

void initialize_byte_array(VARIANTARG& value,
                           const std::span<const std::byte> input) {
  ::VariantInit(&value);
  SAFEARRAY* const array =
      ::SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(input.size()));
  if (array == nullptr) throw std::runtime_error("SAFEARRAY allocation failed");
  value.vt = VT_ARRAY | VT_UI1;
  value.parray = array;

  void* raw = nullptr;
  require(SUCCEEDED(::SafeArrayAccessData(array, &raw)),
          "SAFEARRAY access failed");
  auto* const data = static_cast<std::byte*>(raw);
  for (std::size_t index = 0; index < input.size(); ++index) {
    data[index] = input[index];
  }
  require(SUCCEEDED(::SafeArrayUnaccessData(array)),
          "SAFEARRAY release failed");
}

void require_array_wiped(const VARIANTARG& value,
                         const std::size_t size,
                         const char* const message) {
  void* raw = nullptr;
  require(SUCCEEDED(::SafeArrayAccessData(value.parray, &raw)),
          "SAFEARRAY inspection failed");
  const auto* const data = static_cast<const std::byte*>(raw);
  for (std::size_t index = 0; index < size; ++index) {
    require(data[index] == std::byte{0}, message);
  }
  require(SUCCEEDED(::SafeArrayUnaccessData(value.parray)),
          "SAFEARRAY inspection release failed");
}

class ByteArray final {
 public:
  explicit ByteArray(const std::span<const std::byte> input)
      : size_(input.size()) {
    initialize_byte_array(value_, input);
  }

  ~ByteArray() noexcept { static_cast<void>(::VariantClear(&value_)); }

  ByteArray(const ByteArray&) = delete;
  ByteArray& operator=(const ByteArray&) = delete;

  [[nodiscard]] VARIANTARG& value() noexcept { return value_; }
  void require_wiped(const char* const message) const {
    require_array_wiped(value_, size_, message);
  }

 private:
  VARIANTARG value_{};
  std::size_t size_{};
};

class InitializationArguments final {
 public:
  InitializationArguments(const std::span<const std::byte> password,
                          const std::span<const std::byte> confirmation)
      : password_size_(password.size()),
        confirmation_size_(confirmation.size()) {
    // IDispatch receives positional arguments in reverse order.
    initialize_byte_array(values_[0], confirmation);
    initialize_byte_array(values_[1], password);
  }

  ~InitializationArguments() noexcept {
    static_cast<void>(::VariantClear(&values_[0]));
    static_cast<void>(::VariantClear(&values_[1]));
  }

  InitializationArguments(const InitializationArguments&) = delete;
  InitializationArguments& operator=(const InitializationArguments&) = delete;

  [[nodiscard]] DISPPARAMS parameters() noexcept {
    return DISPPARAMS{values_.data(), nullptr, 2, 0};
  }

  void replace_password_with_bstr() {
    static_cast<void>(::VariantClear(&values_[1]));
    values_[1].vt = VT_BSTR;
    values_[1].bstrVal = ::SysAllocString(L"must not be accepted");
    require(values_[1].bstrVal != nullptr, "test BSTR allocation failed");
    password_is_array_ = false;
  }

  void replace_confirmation_with_bstr() {
    static_cast<void>(::VariantClear(&values_[0]));
    values_[0].vt = VT_BSTR;
    values_[0].bstrVal = ::SysAllocString(L"must not be accepted");
    require(values_[0].bstrVal != nullptr, "test BSTR allocation failed");
    confirmation_is_array_ = false;
  }

  void require_password_wiped() const {
    require(password_is_array_, "password is not an array");
    require_array_wiped(values_[1], password_size_,
                        "initialization password SAFEARRAY survived");
  }

  void require_confirmation_wiped() const {
    require(confirmation_is_array_, "confirmation is not an array");
    require_array_wiped(values_[0], confirmation_size_,
                        "initialization confirmation SAFEARRAY survived");
  }

 private:
  std::array<VARIANTARG, 2> values_{};
  std::size_t password_size_{};
  std::size_t confirmation_size_{};
  bool password_is_array_{true};
  bool confirmation_is_array_{true};
};

class RecordingCloseSink final : public usb_vault::ui::IUiCloseRequestSink {
 public:
  void request_close() noexcept override { ++close_calls; }

  int close_calls{};
};

RecordingCloseSink g_close_sink;

[[nodiscard]] ComPtr<IDispatch> make_host(
    CardProductUiSessionController& controller) {
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
              controller, &g_close_sink, &host)),
          "product host creation failed");
  return host;
}

[[nodiscard]] DISPID member_id(IDispatch& host, wchar_t* const name) {
  LPOLESTR mutable_name = name;
  DISPID id = DISPID_UNKNOWN;
  require(SUCCEEDED(host.GetIDsOfNames(IID_NULL, &mutable_name, 1,
                                       LOCALE_INVARIANT, &id)),
          "product host member was not exposed");
  return id;
}

[[nodiscard]] DISPID member_id(IDispatch& host, const wchar_t* const name) {
  return member_id(host, const_cast<wchar_t*>(name));
}

[[nodiscard]] HRESULT invoke_login(IDispatch& host, VARIANTARG& password,
                                   UINT* const argument_error = nullptr) {
  DISPPARAMS parameters{&password, nullptr, 1, 0};
  return host.Invoke(member_id(host, L"submitLoginPasswordBytes"), IID_NULL,
                     LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                     nullptr, argument_error);
}

[[nodiscard]] HRESULT invoke_initialization(
    IDispatch& host, InitializationArguments& arguments,
    UINT* const argument_error = nullptr) {
  DISPPARAMS parameters = arguments.parameters();
  return host.Invoke(
      member_id(host, L"submitInitializationPasswordBytes"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr, nullptr,
      argument_error);
}

[[nodiscard]] LONG invoke_state(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getSessionState"), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD, &parameters,
                                &result, nullptr, nullptr)),
          "product state query failed");
  require(result.vt == VT_I4, "product state returned the wrong type");
  const LONG state = result.lVal;
  static_cast<void>(::VariantClear(&result));
  return state;
}

[[nodiscard]] bool invoke_mounted(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"hasActiveSession"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "mounted query failed");
  require(result.vt == VT_BOOL, "mounted query returned the wrong type");
  const bool mounted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return mounted;
}

[[nodiscard]] VARIANTARG number_argument(const std::uint32_t value) noexcept {
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_UI4;
  argument.ulVal = value;
  return argument;
}

[[nodiscard]] HRESULT invoke_with_number(IDispatch& host,
                                         const wchar_t* const name,
                                         const std::uint32_t value,
                                         VARIANT* const result,
                                         UINT* const argument_error) {
  VARIANTARG argument = number_argument(value);
  DISPPARAMS parameters{&argument, nullptr, 1, 0};
  return host.Invoke(member_id(host, name), IID_NULL, LOCALE_INVARIANT,
                     DISPATCH_METHOD, &parameters, result, nullptr,
                     argument_error);
}

[[nodiscard]] VARIANTARG boolean_argument(const bool value) noexcept {
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_BOOL;
  argument.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
  return argument;
}

[[nodiscard]] bool invoke_setting(IDispatch& host,
                                  const wchar_t* const name) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, name), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "setting query failed");
  require(result.vt == VT_BOOL, "a setting query returned the wrong type");
  const bool value = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] HRESULT invoke_set_setting(IDispatch& host,
                                         const wchar_t* const name,
                                         const bool value,
                                         UINT* const argument_error) {
  VARIANTARG argument = boolean_argument(value);
  DISPPARAMS parameters{&argument, nullptr, 1, 0};
  const HRESULT status =
      host.Invoke(member_id(host, name), IID_NULL, LOCALE_INVARIANT,
                  DISPATCH_METHOD, &parameters, nullptr, nullptr,
                  argument_error);
  static_cast<void>(::VariantClear(&argument));
  return status;
}

[[nodiscard]] bool invoke_remember(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getRememberContainerPaths"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "remember-paths query failed");
  require(result.vt == VT_BOOL,
          "remember-paths query returned the wrong type");
  const bool remember = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return remember;
}

[[nodiscard]] HRESULT invoke_set_remember(IDispatch& host, const bool value,
                                          UINT* const argument_error) {
  VARIANTARG argument = boolean_argument(value);
  DISPPARAMS parameters{&argument, nullptr, 1, 0};
  const HRESULT status =
      host.Invoke(member_id(host, L"setRememberContainerPaths"), IID_NULL,
                  LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                  nullptr, argument_error);
  static_cast<void>(::VariantClear(&argument));
  return status;
}

// A BSTR argument. The caller owns it and clears it with the rest of the list.
[[nodiscard]] VARIANTARG text_argument(const std::wstring& value) {
  VARIANTARG argument{};
  ::VariantInit(&argument);
  argument.vt = VT_BSTR;
  argument.bstrVal = ::SysAllocStringLen(value.c_str(),
                                         static_cast<UINT>(value.size()));
  require(argument.bstrVal != nullptr, "test BSTR allocation failed");
  return argument;
}

// `createContainerMib(name, directory, sizeMib)` with COM's reverse order.
[[nodiscard]] HRESULT invoke_create_container(
    IDispatch& host, const std::wstring& name, const std::wstring& directory,
    const std::uint32_t size_mib, VARIANT* const result,
    UINT* const argument_error, bool background = false) {
  std::array<VARIANTARG, 3> values{};
  values[2] = text_argument(name);
  values[1] = text_argument(directory);
  values[0] = number_argument(size_mib);
  DISPPARAMS parameters{values.data(), nullptr, 3, 0};
  const HRESULT status =
      host.Invoke(member_id(host, background ? L"beginCreateContainerMib" : L"createContainerMib"), IID_NULL,
                  LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, result,
                  nullptr, argument_error);
  for (VARIANTARG& value : values) {
    static_cast<void>(::VariantClear(&value));
  }
  return status;
}

[[nodiscard]] bool create_container_succeeds(IDispatch& host,
                                             const std::wstring& name,
                                             const std::wstring& directory,
                                             const std::uint32_t size_mib) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_create_container(host, name, directory, size_mib,
                                            &result, nullptr)),
          "container creation call failed");
  require(result.vt == VT_BOOL, "creation returned the wrong type");
  const bool created = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return created;
}

[[nodiscard]] ULONG invoke_container_count(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getContainerCount"),
                                IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                                &parameters, &result, nullptr, nullptr)),
          "container count query failed");
  require(result.vt == VT_UI4, "container count returned the wrong type");
  const ULONG count = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return count;
}

[[nodiscard]] ULONG invoke_operation_count(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"getOperationCount"),
                                IID_NULL,
                                LOCALE_INVARIANT,
                                DISPATCH_METHOD,
                                &parameters,
                                &result,
                                nullptr,
                                nullptr)),
          "operation count query failed");
  require(result.vt == VT_UI4, "operation count returned the wrong type");
  const ULONG count = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return count;
}

[[nodiscard]] LONG invoke_operation_number(IDispatch& host,
                                           const wchar_t* const name,
                                           const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "operation numeric fact query failed");
  require(result.vt == VT_I4, "operation numeric fact returned the wrong type");
  const LONG value = result.lVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] double invoke_operation_time(IDispatch& host,
                                           const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(
              host, L"getOperationCompletedAtUnixMs", index, &result, nullptr)),
          "operation completion time query failed");
  require(result.vt == VT_R8,
          "operation completion time returned the wrong type");
  const double value = result.dblVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] ULONG invoke_container_number(IDispatch& host,
                                            const wchar_t* const name,
                                            const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "container fact query failed");
  require(result.vt == VT_UI4, "container fact returned the wrong type");
  const ULONG value = result.ulVal;
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] bool invoke_container_mounted(IDispatch& host,
                                            const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, L"getContainerMounted", index,
                                       &result, nullptr)),
          "container mounted query failed");
  require(result.vt == VT_BOOL, "container mounted returned the wrong type");
  const bool mounted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return mounted;
}

[[nodiscard]] std::wstring invoke_container_text(IDispatch& host,
                                                 const wchar_t* const name,
                                                 const std::uint32_t index) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, index, &result, nullptr)),
          "container text query failed");
  require(result.vt == VT_BSTR && result.bstrVal != nullptr,
          "container text returned the wrong type");
  const std::wstring value(result.bstrVal, ::SysStringLen(result.bstrVal));
  static_cast<void>(::VariantClear(&result));
  return value;
}

[[nodiscard]] bool invoke_selection(IDispatch& host,
                                    const wchar_t* const name,
                                    const std::uint32_t value) {
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(invoke_with_number(host, name, value, &result, nullptr)),
          "container selection call failed");
  require(result.vt == VT_BOOL, "container selection returned the wrong type");
  const bool selected = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return selected;
}

[[nodiscard]] bool invoke_mount_container(IDispatch& host) {
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(SUCCEEDED(host.Invoke(member_id(host, L"mountContainer"), IID_NULL,
                                LOCALE_INVARIANT, DISPATCH_METHOD, &parameters,
                                &result, nullptr, nullptr)),
          "container adoption call failed");
  require(result.vt == VT_BOOL, "container adoption returned the wrong type");
  const bool adopted = result.boolVal == VARIANT_TRUE;
  static_cast<void>(::VariantClear(&result));
  return adopted;
}

void test_background_creation_reports_progress_and_excludes_concurrent_calls() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.pause_creation.store(true);
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, nullptr, &host, nullptr, &work)), "background host creation failed");
  const HRESULT begun = invoke_create_container(*host.Get(), L"background", fixture.directory.wide(),
      64, nullptr, nullptr, true);
  for (int i = 0; !fixture.volume.creation_entered.load() && i < 1000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  DISPPARAMS no_arguments{};
  VARIANT snapshot{};
  const HRESULT progress_result = host->Invoke(member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr, nullptr);
  const bool live = SUCCEEDED(progress_result) && snapshot.vt == VT_BSTR &&
      std::wstring_view(snapshot.bstrVal) == L"1,3,41,0,3";
  ::VariantClear(&snapshot);
  const HRESULT blocked = host->Invoke(member_id(*host.Get(), L"getSessionState"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr, nullptr);
  fixture.volume.pause_creation.store(false);
  for (int i = 0; work.running.load() && i < 2000; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  require(SUCCEEDED(begun) && live && blocked == E_PENDING && !work.running.load(),
      "background operation did not report live progress or exclude concurrent access");
  require(fixture.volume.create_calls == 1, "background creation ran more than once");
  require(invoke_state(*host.Get()) == 3, "completed job did not release the session");
  fixture.authentication.clear_authentication();
  require(host->Invoke(member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr, nullptr) == E_ACCESSDENIED,
      "completed progress leaked after authentication ended");
}

void test_a_running_creation_can_be_called_off() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.pause_creation.store(true);
  usb_vault::ui::CreationWorkState work;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, nullptr, &host, nullptr, &work)),
      "cancellable host creation failed");
  const HRESULT begun = invoke_create_container(*host.Get(), L"called-off",
      fixture.directory.wide(), 64, nullptr, nullptr, true);
  for (int i = 0; !fixture.volume.creation_entered.load() && i < 1000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  DISPPARAMS no_arguments{};
  VARIANT ignored{};
  // Calling a creation off is one of the two things it leaves reachable; the
  // busy guard would answer E_PENDING to anything else.
  const HRESULT asked = host->Invoke(
      member_id(*host.Get(), L"cancelContainerCreation"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &ignored, nullptr,
      nullptr);
  ::VariantClear(&ignored);
  fixture.volume.pause_creation.store(false);
  for (int i = 0; work.running.load() && i < 2000; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  VARIANT snapshot{};
  const HRESULT progress_result = host->Invoke(
      member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
      LOCALE_INVARIANT, DISPATCH_METHOD, &no_arguments, &snapshot, nullptr,
      nullptr);
  // Status 4 is cancelled: told apart from the failure the page would
  // otherwise report, because nothing went wrong and nothing was made.
  const bool says_cancelled = SUCCEEDED(progress_result) &&
      snapshot.vt == VT_BSTR &&
      std::wstring_view(snapshot.bstrVal).starts_with(L"4,");
  ::VariantClear(&snapshot);
  require(SUCCEEDED(begun) && SUCCEEDED(asked) && work.cancelled.load() &&
          says_cancelled && !work.running.load(),
      "a running creation could not be called off, or did not report it");
}

void test_operation_logging_setting_is_authenticated_and_boolean() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(FAILED(invoke_set_setting(*host.Get(), L"setOperationLoggingEnabled",
      false, nullptr)), "unauthenticated logging setting accepted");
  fixture.authentication.establish(root_secret());
  require(invoke_setting(*host.Get(), L"getOperationLoggingEnabled"),
      "logging default was not projected");
  require(SUCCEEDED(invoke_set_setting(*host.Get(), L"setOperationLoggingEnabled",
      false, nullptr)) && !fixture.volume.logging_enabled,
      "logging preference did not reach the session");
  require(invoke_with_number(*host.Get(), L"setOperationLoggingEnabled", 1,
      nullptr, nullptr) == DISP_E_TYPEMISMATCH,
      "non-boolean logging setting accepted");
}

void test_window_size_presets_are_bounded() {
  class SizeSink final : public usb_vault::ui::IUiWindowSizeSink {
   public:
    bool set_window_size(unsigned int preset) noexcept override {
      selected = preset;
      ++calls;
      return succeeds;
    }
    unsigned int selected{};
    int calls{};
    bool succeeds{true};
  } sink;
  Fixture fixture;
  ComPtr<IDispatch> host;
  require(SUCCEEDED(create_card_product_webview_host_object(
      fixture.controller, nullptr, &host, &sink)), "size host creation failed");
  for (int preset = 0; preset < 4; ++preset) {
    require(SUCCEEDED(invoke_with_number(*host.Get(), L"setWindowSize",
        preset, nullptr, nullptr)) && sink.selected == static_cast<unsigned int>(preset),
        "valid size preset was not applied");
  }
  for (const double invalid : {-1.0, 4.0, 0.5, 4294967296.0}) {
    VARIANTARG argument{};
    argument.vt = VT_R8;
    argument.dblVal = invalid;
    DISPPARAMS parameters{&argument, nullptr, 1, 0};
    require(host->Invoke(member_id(*host.Get(), L"setWindowSize"), IID_NULL,
        LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr, nullptr,
        nullptr) == DISP_E_TYPEMISMATCH, "invalid size accepted");
  }
  require(sink.calls == 4, "invalid size reached window sink");
  sink.succeeds = false;
  require(FAILED(invoke_with_number(*host.Get(), L"setWindowSize", 0,
      nullptr, nullptr)), "window failure was hidden");
  host = make_host(fixture.controller);
  require(FAILED(invoke_with_number(*host.Get(), L"setWindowSize", 0,
      nullptr, nullptr)), "missing size sink claimed success");
}

void test_surface_and_state_projection_are_fixed() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);

  for (const auto [route, expected] : {
           std::pair{std::optional<CardEntryRoute>{}, 0L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::initialization_required},
                     1L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::login_required},
                     2L},
           std::pair{std::optional<CardEntryRoute>{
                         CardEntryRoute::unavailable},
                     4L}}) {
    fixture.entry.route = route;
    require(invoke_state(*host.Get()) == expected,
            "product route mapped to the wrong wire state");
  }
  fixture.authentication.establish(root_secret());
  require(invoke_state(*host.Get()) == 3L,
          "authenticated state mapped to the wrong wire value");

  for (const wchar_t* const forbidden : {
           L"submitPasswordBytes", L"hardwareId", L"credentialRole",
           L"mode", L"tier", L"containerShape", L"volumeType",
           L"rootSecret"}) {
    LPOLESTR name = const_cast<LPOLESTR>(forbidden);
    DISPID id = 0;
    require(host->GetIDsOfNames(IID_NULL, &name, 1, LOCALE_INVARIANT, &id) ==
                    DISP_E_UNKNOWNNAME &&
                id == DISPID_UNKNOWN,
            "product host exposed a forbidden member");
  }
}

void test_login_consumes_and_wipes_one_array() {
  for (const bool reject : {false, true}) {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::login_required;
    fixture.entry.reject_credential = reject;
    fixture.volume.mount_on_login = true;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    std::vector<std::byte> password{
        std::byte{'e'}, std::byte{0xCC}, std::byte{0x81}};
    ByteArray argument(password);

    const HRESULT result = invoke_login(*host.Get(), argument.value());
    argument.require_wiped("login SAFEARRAY survived host invocation");
    require(fixture.entry.login_calls == 1,
            "login host did not forward exactly once");
    if (reject) {
      require(result == E_FAIL && invoke_state(*host.Get()) == 2L,
              "credential rejection lost the retryable login state");
    } else {
      const std::array expected{std::byte{0xC3}, std::byte{0xA9}};
      require(result == S_OK &&
                  equals(fixture.volume.login_bytes, expected) &&
                  invoke_state(*host.Get()) == 3L &&
                  invoke_mounted(*host.Get()),
              "login host changed NFC, state, or mounted semantics");
    }
  }
}

void test_initialization_preserves_com_order_and_wipes_both_arrays() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  std::vector<std::byte> password = bytes("aaaaaaaaaaae");
  password.push_back(std::byte{0xCC});
  password.push_back(std::byte{0x81});
  std::vector<std::byte> confirmation = bytes("aaaaaaaaaaa");
  confirmation.push_back(std::byte{0xC3});
  confirmation.push_back(std::byte{0xA9});
  InitializationArguments arguments(password, confirmation);

  require(invoke_initialization(*host.Get(), arguments) == S_OK,
          "initialization host rejected equivalent NFC inputs");
  arguments.require_password_wiped();
  arguments.require_confirmation_wiped();
  std::vector<std::byte> expected = bytes("aaaaaaaaaaa");
  expected.push_back(std::byte{0xC3});
  expected.push_back(std::byte{0xA9});
  require(fixture.entry.initialization_calls == 1 &&
              equals(fixture.entry.initialization_bytes, expected) &&
              invoke_state(*host.Get()) == 3L,
          "initialization host reversed, changed, or failed inputs");
}

void test_initialization_failure_wipes_every_supported_argument() {
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("bbbbbbbbbbbb");
    InitializationArguments arguments(password, confirmation);

    require(invoke_initialization(*host.Get(), arguments) == E_FAIL,
            "mismatched initialization inputs were accepted");
    arguments.require_password_wiped();
    arguments.require_confirmation_wiped();
    require(fixture.entry.initialization_calls == 0,
            "mismatched inputs reached the card action");
  }
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("aaaaaaaaaaaa");
    InitializationArguments arguments(password, confirmation);
    arguments.replace_password_with_bstr();
    UINT argument_error = 99;

    require(invoke_initialization(*host.Get(), arguments, &argument_error) ==
                    DISP_E_TYPEMISMATCH &&
                argument_error == 1,
            "invalid password argument returned the wrong COM result");
    arguments.require_confirmation_wiped();
    require(fixture.entry.initialization_calls == 0,
            "invalid password argument reached the card action");
  }
  {
    Fixture fixture;
    fixture.entry.route = CardEntryRoute::initialization_required;
    ComPtr<IDispatch> host = make_host(fixture.controller);
    const auto password = bytes("aaaaaaaaaaaa");
    const auto confirmation = bytes("aaaaaaaaaaaa");
    InitializationArguments arguments(password, confirmation);
    arguments.replace_confirmation_with_bstr();
    UINT argument_error = 99;

    require(invoke_initialization(*host.Get(), arguments, &argument_error) ==
                    DISP_E_TYPEMISMATCH &&
                argument_error == 0,
            "invalid confirmation returned the wrong COM result");
    arguments.require_password_wiped();
    require(fixture.entry.initialization_calls == 0,
            "invalid confirmation reached the card action");
  }
}

void test_bad_arity_still_wipes_supplied_password_array() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::initialization_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const auto password = bytes("aaaaaaaaaaaa");
  ByteArray argument(password);
  DISPPARAMS parameters{&argument.value(), nullptr, 1, 0};

  require(host->Invoke(member_id(*host.Get(),
                                 L"submitInitializationPasswordBytes"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &parameters, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "initialization host accepted the wrong argument count");
  argument.require_wiped("bad-arity password SAFEARRAY survived");
}

void test_unmount_and_property_rejection() {
  Fixture fixture;
  fixture.volume.mounted_state = true;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  DISPPARAMS parameters{};

  require(invoke_mounted(*host.Get()), "host hid active volume state");
  require(host->Invoke(member_id(*host.Get(), L"unmount"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, nullptr,
                       nullptr, nullptr) == S_OK &&
              fixture.volume.unmount_calls == 1 &&
              !invoke_mounted(*host.Get()),
          "host did not forward unified unmount");
  require(host->Invoke(member_id(*host.Get(), L"getSessionState"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_PROPERTYGET, &parameters,
                       nullptr, nullptr, nullptr) == DISP_E_MEMBERNOTFOUND,
          "product state was exposed as a property");
}

void test_container_collection_is_exposed_by_position_and_route_gated() {
  Fixture fixture;
  fixture.entry.route = CardEntryRoute::login_required;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(invoke_container_count(*host.Get()) == 0UL,
          "unauthenticated host exposed adopted containers");

  fixture.authentication.establish(root_secret());
  require(create_container_succeeds(*host.Get(), L"work",
                                    fixture.directory.wide(), 64) &&
              fixture.chooser.directory_calls == 0 &&
              fixture.volume.create_calls == 1 &&
              fixture.volume.created_path ==
                  fixture.directory.target(L"work") &&
              fixture.volume.created_size_bytes == 64ULL * 1024ULL * 1024ULL,
          "creation did not carry the named location to the session once");

  require(invoke_container_count(*host.Get()) == 1UL &&
              invoke_container_number(*host.Get(), L"getContainerId", 0) ==
                  1UL &&
              invoke_container_text(*host.Get(), L"getContainerName", 0) ==
                  L"work.hc" &&
              invoke_container_mounted(*host.Get(), 0) &&
              invoke_container_number(*host.Get(), L"getContainerSizeMib",
                                      0) == 64UL &&
              invoke_container_text(*host.Get(), L"getContainerDriveLetter",
                                    0) == L"D:",
          "host lost an adopted container fact");

  VARIANT result{};
  ::VariantInit(&result);
  require(invoke_with_number(*host.Get(), L"getContainerName", 1, &result,
                             nullptr) == E_INVALIDARG,
          "host answered a position no container occupies");
  static_cast<void>(::VariantClear(&result));
}

void test_container_calls_reject_wrong_shapes_and_report_cancellation() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  const std::wstring directory = fixture.directory.wide();
  ComPtr<IDispatch> host = make_host(fixture.controller);

  UINT argument_error = 99;
  DISPPARAMS empty{};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &empty,
                       nullptr, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "host accepted a creation call without any arguments");
  require(invoke_with_number(*host.Get(), L"createContainerMib", 64, nullptr,
                             &argument_error) == DISP_E_BADPARAMCOUNT,
          "host accepted a creation call carrying only a capacity");

  // The two new arguments are strings and nothing else. A number where a name
  // belongs is not coerced into one.
  std::array<VARIANTARG, 3> numeric{};
  numeric[2] = number_argument(7);
  numeric[1] = number_argument(8);
  numeric[0] = number_argument(64);
  DISPPARAMS numeric_parameters{numeric.data(), nullptr, 3, 0};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &numeric_parameters, nullptr, nullptr,
                       &argument_error) == DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "host accepted a number where only a name is allowed");
  for (VARIANTARG& value : numeric) {
    static_cast<void>(::VariantClear(&value));
  }

  require(invoke_create_container(*host.Get(), L"work", directory, 19, nullptr,
                                  &argument_error) == DISP_E_TYPEMISMATCH,
          "host accepted a capacity below the shared minimum");

  // The page owns the location now, so the shapes it must not be able to
  // express are refused here rather than reaching the session.
  for (const std::pair<std::wstring, std::wstring> rejected : {
           std::pair<std::wstring, std::wstring>{L"", directory},
           std::pair<std::wstring, std::wstring>{L"..\\escape", directory},
           std::pair<std::wstring, std::wstring>{L"work", L"Vault"},
           std::pair<std::wstring, std::wstring>{L"work",
                                                 L"\\\\server\\share"},
           std::pair<std::wstring, std::wstring>{L"work",
                                                 directory + L"\\missing"}}) {
    require(invoke_create_container(*host.Get(), rejected.first,
                                    rejected.second, 64, nullptr,
                                    &argument_error) == DISP_E_TYPEMISMATCH,
            "host accepted a location the page must not be able to name");
  }

  std::array<VARIANTARG, 3> fractional{};
  fractional[2] = text_argument(L"work");
  fractional[1] = text_argument(directory);
  ::VariantInit(&fractional[0]);
  fractional[0].vt = VT_R8;
  fractional[0].dblVal = 64.5;
  DISPPARAMS fractional_parameters{fractional.data(), nullptr, 3, 0};
  require(host->Invoke(member_id(*host.Get(), L"createContainerMib"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &fractional_parameters, nullptr, nullptr,
                       &argument_error) == DISP_E_TYPEMISMATCH,
          "host accepted a capacity that is not a whole number");
  for (VARIANTARG& value : fractional) {
    static_cast<void>(::VariantClear(&value));
  }

  require(fixture.chooser.directory_calls == 0 &&
              fixture.volume.create_calls == 0,
          "a rejected creation still opened a dialog or the session");

  require(!invoke_mount_container(*host.Get()) &&
              fixture.chooser.existing_calls == 1,
          "a cancelled adoption was not reported as a cancellation");
}

// The browse button. It reports the choice as a string and creates nothing.
void test_directory_browse_reports_the_choice_as_a_string() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  fixture.chooser.directory_path = L"D:\\Vault";
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(host->Invoke(member_id(*host.Get(), L"chooseContainerDirectory"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &parameters, &result, nullptr, nullptr) == S_OK &&
              result.vt == VT_BSTR && result.bstrVal != nullptr &&
              std::wstring(result.bstrVal, ::SysStringLen(result.bstrVal)) ==
                  L"D:\\Vault",
          "the chosen directory did not reach the page");
  static_cast<void>(::VariantClear(&result));

  fixture.chooser.directory_path.clear();
  ::VariantInit(&result);
  require(host->Invoke(member_id(*host.Get(), L"chooseContainerDirectory"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &parameters, &result, nullptr, nullptr) == S_OK &&
              result.vt == VT_BSTR &&
              ::SysStringLen(result.bstrVal) == 0,
          "a cancelled picker was not reported as an empty string");
  static_cast<void>(::VariantClear(&result));

  VARIANTARG stray = number_argument(1);
  DISPPARAMS stray_parameters{&stray, nullptr, 1, 0};
  require(host->Invoke(member_id(*host.Get(), L"chooseContainerDirectory"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &stray_parameters, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "the picker accepted an argument it does not take");
  static_cast<void>(::VariantClear(&stray));
  require(fixture.chooser.directory_calls == 2 &&
              fixture.volume.create_calls == 0,
          "browsing created something or ran the wrong number of times");
}

void test_the_remember_switch_crosses_the_bridge_as_a_boolean() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);

  // Reading needs no session: the settings page shows the switch in its real
  // position before anyone logs in.
  require(invoke_remember(*host.Get()), "the switch did not read its default");
  require(invoke_set_remember(*host.Get(), false, nullptr) == E_FAIL &&
              fixture.volume.remember_paths_writes == 0,
          "the switch was changed off the authenticated route");

  fixture.authentication.establish(root_secret());
  require(invoke_set_remember(*host.Get(), false, nullptr) == S_OK &&
              fixture.volume.remember_paths_writes == 1 &&
              !invoke_remember(*host.Get()),
          "turning the switch off did not reach the session");
  require(invoke_set_remember(*host.Get(), true, nullptr) == S_OK &&
              invoke_remember(*host.Get()),
          "the switch could not be turned back on");

  // A setting is a boolean and nothing else. A number that C++ would happily
  // read as true must not cross as one.
  UINT argument_error = 99;
  require(invoke_with_number(*host.Get(), L"setRememberContainerPaths", 1,
                             nullptr, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "the switch accepted a number");
  DISPPARAMS empty{};
  require(host->Invoke(member_id(*host.Get(), L"setRememberContainerPaths"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &empty,
                       nullptr, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "the switch accepted a call with nothing to set");
  VARIANTARG stray = boolean_argument(true);
  DISPPARAMS stray_parameters{&stray, nullptr, 1, 0};
  require(host->Invoke(member_id(*host.Get(), L"getRememberContainerPaths"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                       &stray_parameters, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "reading the switch accepted an argument it does not take");
  static_cast<void>(::VariantClear(&stray));
  require(fixture.volume.remember_paths_writes == 2,
          "a refused call still reached the session");
}

void test_the_import_switch_crosses_the_bridge_as_a_boolean() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);

  require(!invoke_setting(*host.Get(), L"getMountImportedContainers"),
          "the import switch did not read its default");
  require(invoke_set_setting(*host.Get(), L"setMountImportedContainers", true,
                             nullptr) == E_FAIL &&
              fixture.volume.mount_imported_writes == 0,
          "the switch was changed off the authenticated route");

  fixture.authentication.establish(root_secret());
  require(invoke_set_setting(*host.Get(), L"setMountImportedContainers", true,
                             nullptr) == S_OK &&
              fixture.volume.mount_imported_writes == 1 &&
              invoke_setting(*host.Get(), L"getMountImportedContainers"),
          "turning the switch on did not reach the session");

  UINT argument_error = 99;
  require(invoke_with_number(*host.Get(), L"setMountImportedContainers", 1,
                             nullptr, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "the switch accepted a number");
  DISPPARAMS empty{};
  require(host->Invoke(member_id(*host.Get(), L"setMountImportedContainers"),
                       IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD, &empty,
                       nullptr, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "the switch accepted a call with nothing to set");
  require(fixture.volume.mount_imported_writes == 1,
          "a refused call still reached the session");
}

void test_adopted_container_mount_and_unmount_route_through_identifiers() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(create_container_succeeds(*host.Get(), L"work",
                                    fixture.directory.wide(), 64),
          "creation was refused");
  const ULONG id = invoke_container_number(*host.Get(), L"getContainerId", 0);

  require(invoke_with_number(*host.Get(), L"unmountContainer", id, nullptr,
                             nullptr) == S_OK &&
              fixture.volume.unmount_container_calls == 1 &&
              !invoke_container_mounted(*host.Get(), 0) &&
              invoke_container_text(*host.Get(), L"getContainerDriveLetter",
                                    0).empty(),
          "unmounting by identifier did not clear the drive");

  require(invoke_selection(*host.Get(), L"mountAdoptedContainer", id) &&
              fixture.chooser.existing_calls == 0 &&
              invoke_container_mounted(*host.Get(), 0),
          "remounting by identifier asked for the file again");

  UINT argument_error = 99;
  require(invoke_with_number(*host.Get(), L"mountAdoptedContainer", id + 7,
                             nullptr, &argument_error) ==
                  DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "host mounted an identifier the session never adopted");
  require(invoke_with_number(*host.Get(), L"unmountContainer", id + 7, nullptr,
                             &argument_error) == DISP_E_TYPEMISMATCH,
          "host unmounted an identifier the session never adopted");

  // Dropping a container off the list is the same shape of call, and the same
  // refusal for an identifier nobody adopted.
  require(invoke_with_number(*host.Get(), L"forgetContainer", id + 7, nullptr,
                             &argument_error) == DISP_E_TYPEMISMATCH,
          "host dropped an identifier the session never adopted");
  require(invoke_with_number(*host.Get(), L"unmountContainer", id, nullptr,
                             nullptr) == S_OK,
          "unmounting before the drop failed");
  require(invoke_with_number(*host.Get(), L"forgetContainer", id, nullptr,
                             nullptr) == S_OK &&
              // The refused attempt above reached the session too, so this is
              // the second call and the first one that changed anything.
              fixture.volume.forget_container_calls == 2 &&
              invoke_container_count(*host.Get()) == 0,
          "dropping by identifier did not take the container off the list");
}

// Builds `createContainerBMib(name, directory, outer, hidden, second,
// confirmation)` in COM's reverse order. The two password arrays stay with the
// caller so it can prove they were wiped.
void fill_container_b_arguments(std::array<VARIANTARG, 6>& values,
                                const std::wstring& name,
                                const std::wstring& directory,
                                const std::span<const std::byte> second) {
  initialize_byte_array(values[0], second);
  initialize_byte_array(values[1], second);
  values[2] = number_argument(16);
  values[3] = number_argument(64);
  values[4] = text_argument(directory);
  values[5] = text_argument(name);
}

void test_container_b_creation_consumes_six_reversed_arguments(bool background = false) {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  const auto login = bytes("login-password");
  const auto second = bytes("second-password-value");

  std::array<VARIANTARG, 6> values{};
  fill_container_b_arguments(values, L"handover", fixture.directory.wide(),
                             second);
  DISPPARAMS parameters{values.data(), nullptr, 6, 0};

  VARIANT created{};
  ::VariantInit(&created);
  const HRESULT result =
      host->Invoke(member_id(*host.Get(), background ? L"beginCreateContainerBMib" : L"createContainerBMib"), IID_NULL,
                   LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, &created,
                   nullptr, nullptr);
  require(result == S_OK && created.vt == VT_BOOL &&
              created.boolVal == VARIANT_TRUE,
          "container B creation was refused");
  static_cast<void>(::VariantClear(&created));
  if (background) {
    bool completed = false;
    for (int i = 0; i < 2000; ++i) {
      DISPPARAMS empty{};
      VARIANT progress{};
      const HRESULT read = host->Invoke(member_id(*host.Get(), L"getCreationProgress"), IID_NULL,
          LOCALE_INVARIANT, DISPATCH_METHOD, &empty, &progress, nullptr, nullptr);
      completed = SUCCEEDED(read) && progress.vt == VT_BSTR && std::wstring_view(progress.bstrVal).starts_with(L"2,");
      ::VariantClear(&progress);
      if (completed) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(completed, "background hidden creation did not complete");
  }
  require(fixture.chooser.directory_calls == 0 &&
              fixture.volume.created_b_path ==
                  fixture.directory.target(L"handover"),
          "container B was not created at the named location");
  require(fixture.entry.second_password_calls == 1 &&
              fixture.entry.existing_password_bytes == login &&
              fixture.entry.second_password_bytes == second,
          "the card record did not receive both passwords");
  require(fixture.volume.create_b_calls == 1 &&
              fixture.volume.created_outer_bytes == 64ULL * 1024ULL * 1024ULL &&
              fixture.volume.created_hidden_bytes ==
                  16ULL * 1024ULL * 1024ULL,
          "the volume transaction received the wrong sizes");
  for (std::size_t index = 0; index < 2; ++index) {
    require_array_wiped(values[index], second.size(),
                        "a container B password array survived the call");
  }
  // The location is not a password, but a container path is exactly what
  // section 8.4 keeps off this machine, so the marshalled copy does not
  // outlive the call either.
  for (std::size_t index = 4; index < 6; ++index) {
    const UINT length = ::SysStringLen(values[index].bstrVal);
    require(length != 0, "a container B location argument lost its length");
    for (UINT position = 0; position < length; ++position) {
      require(values[index].bstrVal[position] == L'\0',
              "a container B location argument survived the call");
    }
  }
  for (VARIANTARG& value : values) {
    static_cast<void>(::VariantClear(&value));
  }

  Fixture short_call;
  short_call.authentication.establish(root_secret());
  ComPtr<IDispatch> short_host = make_host(short_call.controller);
  DISPPARAMS empty{};
  require(short_host->Invoke(member_id(*short_host.Get(),
                                       L"createContainerBMib"),
                             IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
                             &empty, nullptr, nullptr, nullptr) ==
              DISP_E_BADPARAMCOUNT,
          "container B accepted the wrong argument count");
  require(short_call.entry.second_password_calls == 0 &&
              short_call.volume.create_b_calls == 0,
          "a malformed container B call still reached the product");

  // 8.7.2: a rejected location stops this shape exactly where it stops a
  // standard one, before the card record is touched, and the passwords that
  // came with it are still wiped.
  Fixture rejected;
  rejected.authentication.establish(root_secret());
  ComPtr<IDispatch> rejected_host = make_host(rejected.controller);
  std::array<VARIANTARG, 6> rejected_values{};
  fill_container_b_arguments(rejected_values, L"handover", L"Vault", second);
  DISPPARAMS rejected_parameters{rejected_values.data(), nullptr, 6, 0};
  UINT argument_error = 99;
  require(rejected_host->Invoke(
              member_id(*rejected_host.Get(), L"createContainerBMib"),
              IID_NULL, LOCALE_INVARIANT, DISPATCH_METHOD,
              &rejected_parameters, nullptr, nullptr, &argument_error) ==
              DISP_E_TYPEMISMATCH,
          "container B accepted a save location the page must not name");
  require(rejected.entry.second_password_calls == 0 &&
              rejected.volume.create_b_calls == 0,
          "a rejected location still rewrote the card or created a volume");
  for (std::size_t index = 0; index < 2; ++index) {
    require_array_wiped(rejected_values[index], second.size(),
                        "a rejected container B password array survived");
  }
  for (VARIANTARG& value : rejected_values) {
    static_cast<void>(::VariantClear(&value));
  }
}

void test_container_query_failure_does_not_invent_facts() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  fixture.volume.fail_container_statuses = true;

  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(host->Invoke(member_id(*host.Get(), L"getContainerCount"), IID_NULL,
                       LOCALE_INVARIANT, DISPATCH_METHOD, &parameters, &result,
                       nullptr, nullptr) == E_FAIL,
          "a failed container query reported a count anyway");
  require(invoke_with_number(*host.Get(), L"getContainerName", 0, &result,
                             nullptr) == E_FAIL,
          "a failed container query reported a name anyway");
  static_cast<void>(::VariantClear(&result));
}

void test_operation_history_is_read_only_typed_and_clearable() {
  Fixture fixture;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  require(invoke_operation_count(*host.Get()) == 0,
          "an unauthenticated route exposed operation history");

  fixture.authentication.establish(root_secret());
  fixture.volume.operation_events = {
      {.sequence = 1,
       .completed_at_unix_ms = 1788253200000,
       .kind = usb_vault::application::OperationKind::session_opened,
       .outcome = usb_vault::application::OperationOutcome::succeeded},
      {.sequence = 2,
       .completed_at_unix_ms = 1788253201000,
       .kind = usb_vault::application::OperationKind::container_mounted,
       .outcome = usb_vault::application::OperationOutcome::succeeded,
       .container_name = L"work.hc",
       .drive_letter = L'X'},
  };
  require(
      invoke_operation_count(*host.Get()) == 2 &&
          invoke_operation_time(*host.Get(), 1) == 1788253201000.0 &&
          invoke_operation_number(*host.Get(), L"getOperationKind", 1) == 2 &&
          invoke_operation_number(*host.Get(), L"getOperationOutcome", 1) ==
              0 &&
          invoke_container_text(*host.Get(), L"getOperationContainerName", 1) ==
              L"work.hc" &&
          invoke_container_text(*host.Get(), L"getOperationDriveLetter", 1) ==
              L"X:",
      "operation history lost a typed native fact");

  DISPPARAMS no_arguments{};
  require(host->Invoke(member_id(*host.Get(), L"clearOperationHistory"),
                       IID_NULL,
                       LOCALE_INVARIANT,
                       DISPATCH_METHOD,
                       &no_arguments,
                       nullptr,
                       nullptr,
                       nullptr) == S_OK &&
              fixture.volume.clear_operation_history_calls == 1 &&
              invoke_operation_count(*host.Get()) == 0,
          "the clear-history action did not remove the current-session facts");
}

void test_operation_history_query_failures_do_not_invent_facts() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  fixture.volume.fail_operation_history = true;
  ComPtr<IDispatch> host = make_host(fixture.controller);
  DISPPARAMS parameters{};
  VARIANT result{};
  ::VariantInit(&result);
  require(
      host->Invoke(member_id(*host.Get(), L"getOperationCount"),
                   IID_NULL,
                   LOCALE_INVARIANT,
                   DISPATCH_METHOD,
                   &parameters,
                   &result,
                   nullptr,
                   nullptr) == E_FAIL &&
          invoke_with_number(
              *host.Get(), L"getOperationKind", 0, &result, nullptr) == E_FAIL,
      "a failed history query returned a substitute fact");
  static_cast<void>(::VariantClear(&result));
}

// Every file-manager call takes its own shape, so the tests build the argument
// array themselves rather than hiding COM's reverse order behind a helper.
[[nodiscard]] HRESULT invoke_named(IDispatch& host, const wchar_t* const name,
                                   VARIANTARG* const values, const UINT count,
                                   VARIANT* const result,
                                   UINT* const argument_error) {
  DISPPARAMS parameters{values, nullptr, count, 0};
  return host.Invoke(member_id(host, name), IID_NULL, LOCALE_INVARIANT,
                     DISPATCH_METHOD, &parameters, result, nullptr,
                     argument_error);
}

void clear_arguments(VARIANTARG* const values, const UINT count) {
  for (UINT index = 0; index < count; ++index) {
    static_cast<void>(::VariantClear(&values[index]));
  }
}

// 🔴 One call brings a whole directory back. Five hundred files through the
// per-entry accessors above would be two thousand serialized asynchronous
// calls, and the page would visibly stall.
void test_a_directory_listing_arrives_as_one_packed_string() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  fixture.volume.listing.truncated = true;
  fixture.volume.listing.entries.push_back({.name = L"notes.txt",
                                            .is_directory = false,
                                            .is_reparse_point = false,
                                            .size_bytes = 1234,
                                            .modified_unix_ms = 1700000000000LL});
  fixture.volume.listing.entries.push_back({.name = L"elsewhere",
                                            .is_directory = true,
                                            .is_reparse_point = true,
                                            .size_bytes = 0,
                                            .modified_unix_ms = 1700000000001LL});

  std::array<VARIANTARG, 2> values{};
  values[1] = number_argument(7);
  values[0] = text_argument(L"reports");
  VARIANT result{};
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"listContainerDirectory", values.data(), 2,
                       &result, nullptr) == S_OK &&
              result.vt == VT_BSTR && result.bstrVal != nullptr,
          "the listing did not come back as one string");

  const std::wstring packed(result.bstrVal, ::SysStringLen(result.bstrVal));
  const std::wstring expected =
      std::wstring(L"1") + L'\x1e' + L"notes.txt" + L'\x1f' + L'0' + L'\x1f' +
      L'0' + L'\x1f' + L"1234" + L'\x1f' + L"1700000000000" + L'\x1e' +
      L"elsewhere" + L'\x1f' + L'1' + L'\x1f' + L'1' + L'\x1f' + L'0' +
      L'\x1f' + L"1700000000001";
  require(packed == expected, "the packed listing is not what the page parses");
  static_cast<void>(::VariantClear(&result));

  require(fixture.volume.listed.size() == 1 &&
              fixture.volume.listed[0].id == 7 &&
              fixture.volume.listed[0].directory == L"reports",
          "the bridge did not carry the container and the directory");
  // The directory a page is looking at is the content of an encrypted volume,
  // so the marshalled copy does not outlive the call.
  require(::SysStringLen(values[0].bstrVal) == 7 &&
              std::wstring(values[0].bstrVal, 7) == std::wstring(7, L'\0'),
          "the directory the page named outlived the call");
  clear_arguments(values.data(), 2);

  // An empty directory is still a header record, so the page can tell it from
  // a call that returned nothing.
  Fixture empty_fixture;
  empty_fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> empty_host = make_host(empty_fixture.controller);
  std::array<VARIANTARG, 2> empty_values{};
  empty_values[1] = number_argument(1);
  empty_values[0] = text_argument(L"");
  ::VariantInit(&result);
  require(invoke_named(*empty_host.Get(), L"listContainerDirectory",
                       empty_values.data(), 2, &result, nullptr) == S_OK &&
              result.vt == VT_BSTR &&
              std::wstring(result.bstrVal, ::SysStringLen(result.bstrVal)) ==
                  L"0",
          "an empty directory did not come back as a header alone");
  static_cast<void>(::VariantClear(&result));
  clear_arguments(empty_values.data(), 2);
}

void test_the_file_manager_bridge_checks_its_shapes() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  std::array<VARIANTARG, 1> too_few{};
  too_few[0] = number_argument(1);
  require(invoke_named(*host.Get(), L"listContainerDirectory", too_few.data(),
                       1, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a listing with the wrong argument count was accepted");
  clear_arguments(too_few.data(), 1);

  std::array<VARIANTARG, 2> wrong_types{};
  wrong_types[1] = text_argument(L"7");
  wrong_types[0] = text_argument(L"reports");
  UINT argument_error = 99;
  require(invoke_named(*host.Get(), L"listContainerDirectory",
                       wrong_types.data(), 2, nullptr,
                       &argument_error) == DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "a container identifier arrived as text and was accepted");
  clear_arguments(wrong_types.data(), 2);

  std::array<VARIANTARG, 5> wrong_arity{};
  for (UINT index = 0; index < 5; ++index) {
    wrong_arity[index] = number_argument(index);
  }
  require(invoke_named(*host.Get(), L"deleteContainerEntry", wrong_arity.data(),
                       5, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a delete with five arguments was accepted");
  require(invoke_named(*host.Get(), L"moveContainerEntry", wrong_arity.data(),
                       4, nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a move with four arguments was accepted");
  clear_arguments(wrong_arity.data(), 5);

  require(fixture.volume.file_manager_calls() == 0,
          "a rejected call still reached the session");
}

void test_the_bridge_carries_each_file_manager_action() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  VARIANT result{};

  std::array<VARIANTARG, 3> three{};
  three[2] = number_argument(3);
  three[1] = text_argument(L"reports");
  three[0] = text_argument(L"old.txt");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"deleteContainerEntry", three.data(), 3,
                       &result, nullptr) == S_OK &&
              result.vt == VT_BOOL && result.boolVal == VARIANT_TRUE,
          "the delete did not report that it happened");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.deleted.size() == 1 &&
              fixture.volume.deleted[0].id == 3 &&
              fixture.volume.deleted[0].directory == L"reports" &&
              fixture.volume.deleted[0].name == L"old.txt",
          "the delete did not cross the bridge in order");
  clear_arguments(three.data(), 3);

  std::array<VARIANTARG, 3> make{};
  make[2] = number_argument(3);
  make[1] = text_argument(L"");
  make[0] = text_argument(L"archive");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"createContainerDirectory", make.data(), 3,
                       &result, nullptr) == S_OK,
          "the new directory did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.created_directories.size() == 1 &&
              fixture.volume.created_directories[0].name == L"archive",
          "the new directory arrived under another name");
  clear_arguments(make.data(), 3);

  std::array<VARIANTARG, 4> four{};
  four[3] = number_argument(3);
  four[2] = text_argument(L"reports");
  four[1] = text_argument(L"old.txt");
  four[0] = text_argument(L"new.txt");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"renameContainerEntry", four.data(), 4,
                       &result, nullptr) == S_OK,
          "the rename did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.renamed.size() == 1 &&
              fixture.volume.renamed[0].name == L"old.txt" &&
              fixture.volume.renamed[0].new_name == L"new.txt",
          "the rename lost one of its two names");
  clear_arguments(four.data(), 4);

  std::array<VARIANTARG, 5> five{};
  five[4] = number_argument(3);
  five[3] = text_argument(L"reports");
  five[2] = text_argument(L"q1.txt");
  five[1] = number_argument(4);
  five[0] = text_argument(L"archive");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"moveContainerEntry", five.data(), 5,
                       &result, nullptr) == S_OK,
          "the move did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.moved.size() == 1 &&
              fixture.volume.moved[0].source_id == 3 &&
              fixture.volume.moved[0].source_directory == L"reports" &&
              fixture.volume.moved[0].name == L"q1.txt" &&
              fixture.volume.moved[0].destination_id == 4 &&
              fixture.volume.moved[0].destination_directory == L"archive",
          "the move arrived with its two ends confused");
  clear_arguments(five.data(), 5);

  std::array<VARIANTARG, 5> copy{};
  copy[4] = number_argument(4);
  copy[3] = text_argument(L"archive");
  copy[2] = text_argument(L"q1.txt");
  copy[1] = number_argument(3);
  copy[0] = text_argument(L"");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"copyContainerEntry", copy.data(), 5,
                       &result, nullptr) == S_OK,
          "the copy did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.copied.size() == 1 &&
              fixture.volume.copied[0].source_id == 4 &&
              fixture.volume.copied[0].destination_id == 3 &&
              fixture.volume.copied[0].destination_directory.empty(),
          "the copy arrived with its two ends confused");
  clear_arguments(copy.data(), 5);
}

// The two exports cross the bridge like everything else: the container by
// identifier, and the location as text the session checks again.
void test_the_bridge_carries_both_exports() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);
  VARIANT result{};

  std::array<VARIANTARG, 4> entry{};
  entry[3] = number_argument(3);
  entry[2] = text_argument(L"reports");
  entry[1] = text_argument(L"q1.txt");
  entry[0] = text_argument(L"D:\\Out");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"exportContainerEntry", entry.data(), 4,
                       &result, nullptr) == S_OK &&
              result.vt == VT_BOOL && result.boolVal == VARIANT_TRUE,
          "the plaintext export did not report that it happened");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.exported_entries.size() == 1 &&
              fixture.volume.exported_entries[0].id == 3 &&
              fixture.volume.exported_entries[0].directory == L"reports" &&
              fixture.volume.exported_entries[0].name == L"q1.txt" &&
              fixture.volume.exported_entries[0].destination == L"D:\\Out",
          "the plaintext export crossed the bridge out of order");
  // The location is a path on this machine and the name is the content of an
  // encrypted volume, so neither outlives the call.
  require(::SysStringLen(entry[0].bstrVal) == 6 &&
              std::wstring(entry[0].bstrVal, 6) == std::wstring(6, L'\0'),
          "the export location outlived the call");
  clear_arguments(entry.data(), 4);

  std::array<VARIANTARG, 2> file{};
  file[1] = number_argument(4);
  file[0] = text_argument(L"D:\\Migration");
  ::VariantInit(&result);
  require(invoke_named(*host.Get(), L"exportContainerFile", file.data(), 2,
                       &result, nullptr) == S_OK,
          "the migration export did not cross the bridge");
  static_cast<void>(::VariantClear(&result));
  require(fixture.volume.exported_files.size() == 1 &&
              fixture.volume.exported_files[0].id == 4 &&
              fixture.volume.exported_files[0].destination == L"D:\\Migration",
          "the migration export crossed the bridge out of order");
  clear_arguments(file.data(), 2);
}

void test_the_export_bridge_checks_its_shapes() {
  Fixture fixture;
  fixture.authentication.establish(root_secret());
  ComPtr<IDispatch> host = make_host(fixture.controller);

  std::array<VARIANTARG, 4> values{};
  values[3] = number_argument(3);
  values[2] = text_argument(L"reports");
  values[1] = text_argument(L"q1.txt");
  values[0] = text_argument(L"D:\\Out");
  require(invoke_named(*host.Get(), L"exportContainerEntry", values.data(), 3,
                       nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a plaintext export with three arguments was accepted");
  require(invoke_named(*host.Get(), L"exportContainerFile", values.data(), 4,
                       nullptr, nullptr) == DISP_E_BADPARAMCOUNT,
          "a migration export with four arguments was accepted");
  clear_arguments(values.data(), 4);

  std::array<VARIANTARG, 2> wrong_types{};
  wrong_types[1] = text_argument(L"4");
  wrong_types[0] = text_argument(L"D:\\Migration");
  UINT argument_error = 99;
  require(invoke_named(*host.Get(), L"exportContainerFile", wrong_types.data(),
                       2, nullptr, &argument_error) == DISP_E_TYPEMISMATCH &&
              argument_error == 0,
          "a container identifier arrived as text and was accepted");
  clear_arguments(wrong_types.data(), 2);

  require(fixture.volume.exported_entries.empty() &&
              fixture.volume.exported_files.empty(),
          "a rejected export still reached the session");
}

}  // namespace

int main() {
  try {
    test_surface_and_state_projection_are_fixed();
    test_window_size_presets_are_bounded();
    test_operation_logging_setting_is_authenticated_and_boolean();
    test_background_creation_reports_progress_and_excludes_concurrent_calls();
    test_a_running_creation_can_be_called_off();
    test_login_consumes_and_wipes_one_array();
    test_initialization_preserves_com_order_and_wipes_both_arrays();
    test_initialization_failure_wipes_every_supported_argument();
    test_bad_arity_still_wipes_supplied_password_array();
    test_unmount_and_property_rejection();
    test_container_collection_is_exposed_by_position_and_route_gated();
    test_container_calls_reject_wrong_shapes_and_report_cancellation();
    test_directory_browse_reports_the_choice_as_a_string();
    test_adopted_container_mount_and_unmount_route_through_identifiers();
    test_the_remember_switch_crosses_the_bridge_as_a_boolean();
    test_the_import_switch_crosses_the_bridge_as_a_boolean();
    test_container_b_creation_consumes_six_reversed_arguments();
    test_container_b_creation_consumes_six_reversed_arguments(true);
    test_container_query_failure_does_not_invent_facts();
    test_operation_history_is_read_only_typed_and_clearable();
    test_operation_history_query_failures_do_not_invent_facts();
    test_a_directory_listing_arrives_as_one_packed_string();
    test_the_file_manager_bridge_checks_its_shapes();
    test_the_bridge_carries_each_file_manager_action();
    test_the_bridge_carries_both_exports();
    test_the_export_bridge_checks_its_shapes();
    std::cout << "card_product_webview_host_object_tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "card_product_webview_host_object_tests failed: "
              << error.what() << '\n';
    return 1;
  }
}
