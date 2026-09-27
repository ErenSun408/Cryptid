#include "usb_vault/application/card_login_mount_session.h"

#include "application/card_login_mount_session_internal.h"
#include "usb_vault/application/card_authentication_session.h"
#include "usb_vault/application/container_index_store_v1.h"
#include "usb_vault/application/container_index_view_v1.h"
#include "usb_vault/application/container_layout_v1.h"
#include "usb_vault/application/product_settings_v1.h"
#include "usb_vault/security/kdf_v1.h"
#include "usb_vault/volume/unmount_retry.h"
#include "usb_vault/volume/volume_entry_path.h"
#include "usb_vault/volume/volume_file_operations.h"
#include "usb_vault/volume_creation/volume_creator.h"
#include "volume/drive_letter_pool.h"
#include "volume/mount_request.h"

#include <Windows.h>

#include "usb_vault/diagnostics/debug_log.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace usb_vault::application {
namespace {

class ProductionCardLoginMountSessionRuntime final
    : public detail::CardLoginMountSessionRuntime {
 public:
  explicit ProductionCardLoginMountSessionRuntime(
      const volume::VolumeProvider& provider,
      std::filesystem::path volume_creator_helper_path)
      : provider_(provider),
        creator_(volume_creator_helper_path),
        imports_(std::move(volume_creator_helper_path)) {}

  [[nodiscard]] volume::UnmountResult unmount(
      const wchar_t drive_letter) override {
    return volume::unmount_with_retry(provider_, drive_letter);
  }

  [[nodiscard]] volume::UnmountEscalationResult unmount_forced(
      const wchar_t drive_letter) override {
    return volume::unmount_with_forced_escalation(provider_, drive_letter);
  }

  [[nodiscard]] bool container_exists(
      const std::wstring_view path) override {
    return container_size_if_present(path).has_value();
  }

  [[nodiscard]] std::optional<std::uint64_t> container_size_if_present(
      const std::wstring_view path) override {
    volume::detail::validate_volume_path(path);
    const std::wstring owned_path(path);
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (::GetFileAttributesExW(owned_path.c_str(), GetFileExInfoStandard,
                               &attributes) == FALSE) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return std::nullopt;
      }
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "inspect container status path");
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      throw std::invalid_argument(
          "container status path refers to a directory");
    }
    if ((attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      throw std::invalid_argument(
          "container status path must not be a reparse point");
    }
    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    return size.QuadPart;
  }

  [[nodiscard]] std::optional<std::uint64_t> mounted_volume_size(
      const wchar_t drive_letter) override {
    for (const volume::MountedVolume& mounted : provider_.mounted_volumes()) {
      if (mounted.drive_letter == drive_letter) return mounted.disk_length;
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<volume::VolumeType> mounted_volume_type(
      const wchar_t drive_letter) override {
    for (const volume::MountedVolume& mounted : provider_.mounted_volumes()) {
      if (mounted.drive_letter == drive_letter) return mounted.type;
    }
    return std::nullopt;
  }

  [[nodiscard]] volume::VolumeSpace volume_space(
      const wchar_t drive_letter) override {
    return volume::volume_space(drive_letter);
  }

  [[nodiscard]] volume::MountIdentity mount_identity(wchar_t drive) override {
    return provider_.mount_identity(drive);
  }

  [[nodiscard]] std::uint64_t measure_protected_space(
      std::wstring_view path, const volume::MountIdentity& identity,
      const security::SecureBuffer& outer,
      const security::SecureBuffer& hidden) override {
    return imports_.protected_space(path, identity.drive_letter, identity.unique_id,
                                         identity.disk_length, outer, hidden);
  }

  void end_measurements() noexcept override { imports_.close(); }
  void end_imports() noexcept override { imports_.close(); }
  void import_protected(std::wstring_view container, const volume::MountIdentity& identity,
      const security::SecureBuffer& outer, const security::SecureBuffer& hidden,
      std::wstring_view source, std::wstring_view destination,
      const volume::TransferCancellation& cancelled, const std::function<void(unsigned int)>& progress) override {
    imports_.copy(container, identity.drive_letter, identity.unique_id, identity.disk_length,
                  outer, hidden, source, destination, [&](const auto& value) { if (progress) progress(value.percent); }, cancelled);
  }

  [[nodiscard]] security::SecureBuffer derive_container_password(
      const security::SecureBuffer& password_input,
      const security::SecureBuffer& root_secret,
      const security::KdfV1Salt& salt) override {
    return security::derive_volume_password_v1(password_input, root_secret,
                                                salt);
  }

  void format_partition(const volume::PartitionCandidate& target,
      const security::SecureBuffer& volume_password,
      const std::function<bool()>& confirm_erase,
      const CreationCancellation& cancelled) override {
    creator_.format_partition(target, volume_password, confirm_erase,
        [this](const volume_creation::CreationProgress& progress) {
          report_creation(progress, 0, 0);
        }, cancelled);
  }

  void format_partition_pair(const volume::PartitionCandidate& target, std::uint64_t hidden,
      const security::SecureBuffer& outer, const security::SecureBuffer& inner,
      const std::function<bool()>& confirm, const CreationCancellation& cancelled) override {
    creator_.format_partition_pair(target, hidden, outer, inner, confirm,
        [this](const auto& progress) { report_creation(progress, 0, 0); }, cancelled);
  }

  bool convert_partition(const volume::PartitionCandidate& target, bool begin, const security::SecureBuffer& password,
      const std::function<bool()>& confirm, const CreationCancellation& revoked, const CreationCancellation& pause) override {
    return creator_.convert_partition(target,begin,password,confirm,
        [this](const auto& progress){report_creation(progress,0,0);},revoked,pause);
  }

  volume::MountResult mount_partition(const volume::PartitionIdentity& identity,
      const security::SecureBuffer& password, const volume::MountOptions& options) override {
    return provider_.mount_partition(identity, password, options);
  }

  void create_standard(
      const std::wstring_view target_path, const std::uint64_t size_bytes,
      const security::SecureBuffer& volume_password) override {
    creator_.create_standard(target_path, size_bytes, volume_password,
        [this](const volume_creation::CreationProgress& progress) {
          report_creation(progress, 0, 0);
        },
        creation_cancelled);
  }

  void create_with_hidden(
      const std::wstring_view target_path,
      const std::uint64_t outer_size_bytes,
      const std::uint64_t hidden_size_bytes,
      const security::SecureBuffer& outer_password,
      const security::SecureBuffer& hidden_password) override {
    creator_.create_with_hidden(target_path, outer_size_bytes,
                                hidden_size_bytes, outer_password,
                                hidden_password,
        [this, outer_size_bytes, hidden_size_bytes](const volume_creation::CreationProgress& progress) {
          report_creation(progress, outer_size_bytes, hidden_size_bytes);
        },
        creation_cancelled);
  }

  void publish_created(const std::wstring_view staging_path,
                       const std::wstring_view target_path) override {
    const std::wstring staging(staging_path);
    const std::wstring target(target_path);
    if (::MoveFileExW(staging.c_str(), target.c_str(),
                      MOVEFILE_WRITE_THROUGH) == FALSE) {
      throw std::system_error(static_cast<int>(::GetLastError()),
                              std::system_category(),
                              "publish created container");
    }
  }

  void remove_incomplete(const std::wstring_view staging_path) override {
    const std::wstring staging(staging_path);
    if (::DeleteFileW(staging.c_str()) != FALSE) return;
    const DWORD error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "remove incomplete container");
    }
  }

  [[nodiscard]] volume::MountResult mount_created(
      const std::wstring_view path,
      const security::SecureBuffer& volume_password,
      const volume::MountOptions& options) override {
    return provider_.mount_file(path, volume_password, options);
  }

  [[nodiscard]] wchar_t allocate_drive_letter() override {
    DWORD drives = ::GetLogicalDrives();
    if (drives == 0) {
      const DWORD error = ::GetLastError();
      USB_VAULT_TRACE("盘符", "读取系统盘符失败", "Win32 " + std::to_string(error));
      throw std::system_error(static_cast<int>(error),
                              std::system_category(),
                              "inspect available drive letters");
    }
    const DWORD logical = drives;
    // Driver-only mounts also occupy their requested letters; a collision
    // must not be offered merely because DOS has not published the mount.
    drives = volume::occupied_drive_letters(drives, provider_.mounted_volumes());
    const wchar_t letter =
        volume::choose_free_drive_letter(static_cast<std::uint32_t>(drives));
    USB_VAULT_TRACE("盘符", "选择空闲盘符",
                    "系统已用 " + diagnostics::hex_text(logical) + "；含驱动已用 " +
                        diagnostics::hex_text(drives) + "；选中 " + diagnostics::drive_text(letter));
    if (letter) return letter;
    throw std::system_error(
        std::make_error_code(std::errc::no_space_on_device),
        "no free drive letter is available for a container mount");
  }

  void notify_mounted(const wchar_t drive_letter) override {
    provider_.notify_mounted(drive_letter);
  }

  [[nodiscard]] std::vector<std::byte> read_container_index() override {
    const std::wstring path = container_index_v1_default_path();
    static_cast<void>(ensure_container_index_v1_file(path));
    return read_container_index_v1_file(path);
  }

  void write_container_index(
      const std::span<const std::byte> contents) override {
    write_container_index_v1_file(container_index_v1_default_path(), contents);
  }

  [[nodiscard]] bool remember_container_paths() override {
    return remember_container_paths_v1(product_settings_v1_default_path());
  }

  void set_remember_container_paths(const bool remember) override {
    set_remember_container_paths_v1(product_settings_v1_default_path(),
                                    remember);
  }

  [[nodiscard]] bool mount_imported_containers() override {
    return mount_imported_containers_v1(product_settings_v1_default_path());
  }
  bool force_unmount_enabled() override {
    return product_preference_v1(product_settings_v1_default_path(), ProductPreference::force_unmount) != 0;
  }
  bool auto_mount_enabled() override {
    return product_preference_v1(product_settings_v1_default_path(), ProductPreference::auto_mount) != 0;
  }
  void set_auto_mount_enabled(bool enabled) override {
    set_product_preference_v1(product_settings_v1_default_path(), ProductPreference::auto_mount, enabled ? 1U : 0U);
  }
  void set_force_unmount_enabled(bool enabled) override {
    set_product_preference_v1(product_settings_v1_default_path(), ProductPreference::force_unmount, enabled ? 1U : 0U);
  }
  bool operation_logging_enabled() override {
    return product_preference_v1(product_settings_v1_default_path(), ProductPreference::operation_logging) != 0;
  }
  void set_operation_logging_enabled(bool enabled) override {
    set_product_preference_v1(product_settings_v1_default_path(), ProductPreference::operation_logging, enabled ? 1U : 0U);
  }

  void set_mount_imported_containers(const bool mount) override {
    set_mount_imported_containers_v1(product_settings_v1_default_path(),
                                     mount);
  }

  [[nodiscard]] volume::VolumeDirectoryListing list_directory(
      const std::wstring_view absolute_directory) override {
    return volume::list_volume_directory(absolute_directory);
  }

  void delete_entry(const std::wstring_view absolute_path) override {
    volume::delete_volume_entry(absolute_path);
  }

  void create_directory(const std::wstring_view absolute_path) override {
    volume::create_volume_directory(absolute_path);
  }

  void move_entry(const std::wstring_view absolute_source,
                  const std::wstring_view absolute_destination) override {
    volume::move_volume_entry(absolute_source, absolute_destination);
  }

  void move_entry_by_copying(
      const std::wstring_view absolute_source,
      const std::wstring_view absolute_destination) override {
    volume::move_volume_entry_by_copying(absolute_source, absolute_destination);
  }

  void copy_entry(const std::wstring_view absolute_source,
                  const std::wstring_view absolute_destination,
                  const volume::TransferCancellation& cancelled) override {
    volume::copy_volume_entry(absolute_source, absolute_destination, cancelled);
  }

 private:
  void report_creation(const volume_creation::CreationProgress& progress,
                       std::uint64_t outer, std::uint64_t inner) {
    if (!creation_feedback) return;
    using Stage = volume_creation::CreationStage;
    if (progress.stage == Stage::authorized) { creation_feedback(1, 100, 0); return; }
    if (progress.stage == Stage::allocated) { creation_feedback(2, 100, 0); return; }
    unsigned int percent = progress.percent;
    if (outer != 0 && inner != 0) {
      const auto total = outer + inner;
      if (progress.stage == Stage::formatting_outer) percent = static_cast<unsigned int>(outer * progress.percent / total);
      if (progress.stage == Stage::scanning_tail_space) percent = static_cast<unsigned int>(outer * 100 / total);
      if (progress.stage == Stage::formatting_hidden) percent = static_cast<unsigned int>((outer * 100 + inner * progress.percent) / total);
    }
    creation_feedback(3, percent, 0);
  }
  const volume::VolumeProvider& provider_;
  volume_creation::VolumeCreator creator_;
  volume_creation::VolumeImportSession imports_;
};

}  // namespace

ContainerCreationCleanupError::ContainerCreationCleanupError(
    std::exception_ptr operation_failure,
    std::exception_ptr cleanup_failure)
    : std::runtime_error(
          "container creation failed and incomplete-file cleanup failed"),
      operation_failure_(std::move(operation_failure)),
      cleanup_failure_(std::move(cleanup_failure)) {}

DecoyMaintenanceRefused::DecoyMaintenanceRefused()
    : std::runtime_error("this container cannot be opened for maintenance") {}

const std::exception_ptr&
ContainerCreationCleanupError::operation_failure() const noexcept {
  return operation_failure_;
}

const std::exception_ptr&
ContainerCreationCleanupError::cleanup_failure() const noexcept {
  return cleanup_failure_;
}

namespace {

// Test-phase runtime log for the public session calls: the start, the result
// and every failure, with both halves of a creation cleanup failure. Details
// are built only when the switch is on; logging never changes the outcome.
constexpr std::string_view kInterfaceArea = "会话接口";

void note_call(const char* const event, const char* const phase,
               const std::string& detail) noexcept {
  try {
    diagnostics::debug_log(kInterfaceArea, std::string(event) + "：" + phase, detail);
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
  }
}

// Call only inside a catch block.
void note_call_failure(const char* const event, const std::string& detail) noexcept {
  try {
    diagnostics::debug_log_exception(kInterfaceArea, std::string(event) + "：失败", detail);
    try {
      throw;
    } catch (const ContainerCreationCleanupError& error) {
      USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
      diagnostics::debug_log(kInterfaceArea, std::string(event) + "：原操作失败",
                             diagnostics::describe_exception(error.operation_failure()));
      diagnostics::debug_log(kInterfaceArea, std::string(event) + "：清理失败",
                             diagnostics::describe_exception(error.cleanup_failure()));
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    }
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
  }
}

template <typename Result>
[[nodiscard]] std::string call_result_text(const Result& result) {
  if constexpr (std::is_same_v<Result, bool>) {
    return std::string("结果 ") + (result ? "是" : "否");
  } else if constexpr (std::is_same_v<Result, std::uint32_t>) {
    return "结果 容器 #" + std::to_string(result);
  } else if constexpr (std::is_same_v<Result, wchar_t>) {
    return result >= L'A' && result <= L'Z'
               ? "结果 盘符 " + std::string(1, static_cast<char>(result)) + ":"
               : std::string("结果 无盘符");
  } else if constexpr (std::is_same_v<Result, DecoyMaintenanceSurvey>) {
    return "可维护 " + std::to_string(result.maintainable.size()) + "；无法判断 " +
           std::to_string(result.unreachable) + "；已取消 " + (result.cancelled ? "是" : "否");
  } else if constexpr (std::is_same_v<Result, DecoyMaintenanceSpace>) {
    return "总量 " + std::to_string(result.total_bytes) + " 字节；剩余 " +
           std::to_string(result.free_bytes) + " 字节；安全可写 " +
           std::to_string(result.safe_free_bytes) + " 字节";
  } else if constexpr (std::is_same_v<Result, volume::VolumeDirectoryListing>) {
    return "条目数 " + std::to_string(result.entries.size()) + "；已截断 " +
           (result.truncated ? "是" : "否");
  } else {
    return {};
  }
}

template <typename Detail, typename Operation>
auto traced_call(const char* const event, Detail&& detail, Operation&& operation)
    -> decltype(operation()) {
  using Result = decltype(operation());
  std::string text;
  if constexpr (USB_VAULT_DEBUG_LOG) {
    try {
      text = detail();
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    }
    note_call(event, "开始", text);
  }
  try {
    if constexpr (std::is_void_v<Result>) {
      operation();
      if constexpr (USB_VAULT_DEBUG_LOG) note_call(event, "完成", text);
    } else {
      Result result = operation();
      if constexpr (USB_VAULT_DEBUG_LOG) {
        std::string finished = text;
        try {
          const std::string extra = call_result_text(result);
          if (!extra.empty()) finished += (finished.empty() ? "" : "；") + extra;
        } catch (...) {
          USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
        }
        note_call(event, "完成", finished);
      }
      return result;
    }
  } catch (...) {
    if constexpr (USB_VAULT_DEBUG_LOG) note_call_failure(event, text);
    throw;
  }
}

[[nodiscard]] std::string call_text(const std::wstring_view text) {
  return diagnostics::utf8(text);
}
[[nodiscard]] std::string target_text(const volume::PartitionCandidate& target) {
  return "磁盘 " + std::to_string(target.disk_number) + "；分区 " +
         std::to_string(target.partition_number) + "；偏移 " +
         std::to_string(target.offset_bytes) + "；长度 " +
         std::to_string(target.length_bytes);
}

}  // namespace

struct CardLoginMountSession::Impl final {
  Impl(CardAuthenticationSession& authentication,
       const volume::VolumeProvider& provider,
       ContainerLayout layout,
       std::filesystem::path volume_creator_helper_path)
      : runtime(provider, std::move(volume_creator_helper_path)),
        core(authentication, runtime, std::move(layout)) {}

  ProductionCardLoginMountSessionRuntime runtime;
  detail::CardLoginMountSessionCore core;
};

CardLoginMountSession::CardLoginMountSession(
    CardAuthenticationSession& authentication,
    const volume::VolumeProvider& provider,
    ContainerLayout layout,
    std::filesystem::path volume_creator_helper_path)
    : impl_(std::make_unique<Impl>(authentication, provider,
                                   std::move(layout),
                                   std::move(volume_creator_helper_path))) {}

CardLoginMountSession::~CardLoginMountSession() noexcept = default;

void CardLoginMountSession::set_creation_feedback(CreationFeedback feedback) {
  impl_->runtime.creation_feedback = std::move(feedback);
}

void CardLoginMountSession::set_creation_revocation(CreationCancellation revoked) {
  impl_->runtime.creation_revoked=std::move(revoked);
}
bool CardLoginMountSession::begin_partition_conversion(const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  return traced_call("开始分区原地加密", [&] { return target_text(target); },
                     [&] { return impl_->core.begin_partition_conversion(target, confirm); });
}
bool CardLoginMountSession::resume_partition_conversion(std::uint32_t id) {
  return traced_call("继续分区原地加密", [&] { return "容器 #" + std::to_string(id); },
                     [&] { return impl_->core.resume_partition_conversion(id); });
}

void CardLoginMountSession::set_creation_cancellation(
    CreationCancellation cancelled) {
  impl_->runtime.creation_cancelled = std::move(cancelled);
}

void CardLoginMountSession::consume_authenticated_login(
    security::SecureBuffer root_secret,
    security::SecureBuffer login_password_utf8) {
  traced_call("登录", [] { return std::string(); }, [&] {
    impl_->core.consume_authenticated_login(std::move(root_secret),
                                            std::move(login_password_utf8));
  });
}

void CardLoginMountSession::create_container_a(
    const std::uint64_t size_bytes) {
  traced_call("创建布局容器 A", [&] { return "容量 " + std::to_string(size_bytes) + " 字节"; },
              [&] { impl_->core.create_container_a(size_bytes); });
}

std::uint32_t CardLoginMountSession::create_partition(const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  return traced_call("创建分区容器", [&] { return target_text(target); },
                     [&] { return impl_->core.create_partition(target, confirm); });
}

std::uint32_t CardLoginMountSession::create_partition_pair(const volume::PartitionCandidate& target,
    std::uint64_t hidden, const security::SecureBuffer& decoy,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  return traced_call("创建含隐藏卷的分区容器",
      [&] { return target_text(target) + "；隐藏卷 " + std::to_string(hidden) + " 字节"; },
      [&] {
        if (!hidden || decoy.empty()) throw std::invalid_argument("partition pair inputs required");
        return impl_->core.create_partition(target, confirm, hidden, &decoy);
      });
}

void CardLoginMountSession::format_partition(
    const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm_erase) {
  traced_call("格式化分区", [&] { return target_text(target); },
              [&] { impl_->core.format_partition(target, confirm_erase); });
}

std::uint32_t CardLoginMountSession::create_container(
    const std::wstring_view path, const std::uint64_t size_bytes) {
  return traced_call("创建文件容器",
      [&] { return "路径 " + call_text(path) + "；容量 " + std::to_string(size_bytes) + " 字节"; },
      [&] { return impl_->core.create_container(path, size_bytes); });
}

std::uint32_t CardLoginMountSession::create_container_b(
    const std::wstring_view path,
    const std::uint64_t outer_size_bytes,
    const std::uint64_t hidden_size_bytes,
    const security::SecureBuffer& decoy_password_utf8) {
  return traced_call("创建含隐藏卷的文件容器",
      [&] {
        return "路径 " + call_text(path) + "；外层 " + std::to_string(outer_size_bytes) +
               " 字节；隐藏卷 " + std::to_string(hidden_size_bytes) + " 字节";
      },
      [&] {
        return impl_->core.create_container_b(path, outer_size_bytes, hidden_size_bytes,
                                              decoy_password_utf8);
      });
}

void CardLoginMountSession::complete_initialization(security::SecureBuffer password_utf8) {
  traced_call("初始化交接", [] { return std::string(); },
              [&] { impl_->core.complete_initialization(std::move(password_utf8)); });
}

security::SecureBuffer CardLoginMountSession::copy_session_password() {
  return impl_->core.copy_session_password();
}

std::uint32_t CardLoginMountSession::mount_container(
    const std::wstring_view path) {
  return traced_call("按路径挂载", [&] { return "路径 " + call_text(path); },
                     [&] { return impl_->core.mount_container(path); });
}

void CardLoginMountSession::mount_adopted_container(const std::uint32_t id) {
  traced_call("挂载容器", [&] { return "容器 #" + std::to_string(id); },
              [&] { impl_->core.mount_adopted_container(id); });
}

wchar_t CardLoginMountSession::open_decoy_maintenance(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  return traced_call("打开诱饵维护", [&] { return "容器 #" + std::to_string(id); },
                     [&] { return impl_->core.open_decoy_maintenance(id, decoy_password_utf8); });
}

bool CardLoginMountSession::verify_decoy_maintenance(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  return traced_call("验证诱饵口令", [&] { return "容器 #" + std::to_string(id); },
                     [&] { return impl_->core.verify_decoy_maintenance(id, decoy_password_utf8); });
}

DecoyMaintenanceSurvey CardLoginMountSession::survey_decoy_maintenance(
    const security::SecureBuffer& decoy_password_utf8,
    const SurveyProgress& progress,
    const SurveyCancellation& cancelled) {
  return traced_call("搜索诱饵容器", [] { return std::string(); }, [&] {
    return impl_->core.survey_decoy_maintenance(decoy_password_utf8, progress, cancelled);
  });
}

DecoyMaintenanceSpace CardLoginMountSession::decoy_maintenance_space(
    const std::uint32_t id) {
  return traced_call("测量维护卷空间", [&] { return "容器 #" + std::to_string(id); },
                     [&] { return impl_->core.decoy_maintenance_space(id); });
}

bool CardLoginMountSession::decoy_maintenance_write_blocked(const std::uint32_t id) {
  return impl_->core.decoy_maintenance_write_blocked(id);
}

void CardLoginMountSession::end_decoy_maintenance(const std::uint32_t id) {
  traced_call("结束诱饵维护", [&] { return "容器 #" + std::to_string(id); },
              [&] { impl_->core.end_decoy_maintenance(id); });
}

void CardLoginMountSession::import_external_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view absolute_source,
    const std::function<bool()>& cancelled, const std::function<void(unsigned int)>& progress) {
  traced_call("导入到维护卷",
      [&] {
        return "容器 #" + std::to_string(id) + "；目录 " + call_text(relative_directory) +
               "；来源 " + call_text(absolute_source);
      },
      [&] {
        impl_->core.import_external_entry(id, relative_directory, absolute_source, cancelled,
                                          progress);
      });
}

wchar_t CardLoginMountSession::open_surveyed_maintenance(
    const std::uint32_t id) {
  return traced_call("打开已搜索的维护卷", [&] { return "容器 #" + std::to_string(id); },
                     [&] { return impl_->core.open_surveyed_maintenance(id); });
}

void CardLoginMountSession::unmount_container(const std::uint32_t id) {
  traced_call("卸载容器", [&] { return "容器 #" + std::to_string(id); },
              [&] { impl_->core.unmount_container(id); });
}

void CardLoginMountSession::forget_container(const std::uint32_t id) {
  traced_call("从列表移除", [&] { return "容器 #" + std::to_string(id); },
              [&] { impl_->core.forget_container(id); });
}

std::uint32_t CardLoginMountSession::adopt_container(
    const std::wstring_view path) {
  return traced_call("导入容器", [&] { return "路径 " + call_text(path); },
                     [&] { return impl_->core.adopt_container(path); });
}

bool CardLoginMountSession::mounting_imported_containers() {
  return impl_->core.mounting_imported_containers();
}

void CardLoginMountSession::set_mounting_imported_containers(
    const bool mount) {
  traced_call("设置导入后自动挂载", [&] { return std::string(mount ? "开" : "关"); },
              [&] { impl_->core.set_mounting_imported_containers(mount); });
}

bool CardLoginMountSession::remembering_container_paths() {
  return impl_->core.remembering_container_paths();
}

void CardLoginMountSession::set_remembering_container_paths(
    const bool remember) {
  traced_call("设置记住容器", [&] { return std::string(remember ? "开" : "关"); },
              [&] { impl_->core.set_remembering_container_paths(remember); });
}

volume::VolumeDirectoryListing CardLoginMountSession::list_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory) {
  return traced_call("列出目录",
      [&] { return "容器 #" + std::to_string(id) + "；目录 " + call_text(relative_directory); },
      [&] { return impl_->core.list_container_directory(id, relative_directory); });
}

void CardLoginMountSession::delete_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  traced_call("删除条目",
      [&] {
        return "容器 #" + std::to_string(id) + "；目录 " + call_text(relative_directory) +
               "；名称 " + call_text(name);
      },
      [&] { impl_->core.delete_container_entry(id, relative_directory, name); });
}

void CardLoginMountSession::rename_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name, const std::wstring_view new_name) {
  traced_call("重命名条目",
      [&] {
        return "容器 #" + std::to_string(id) + "；目录 " + call_text(relative_directory) +
               "；名称 " + call_text(name) + "；新名称 " + call_text(new_name);
      },
      [&] { impl_->core.rename_container_entry(id, relative_directory, name, new_name); });
}

void CardLoginMountSession::create_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  traced_call("新建目录",
      [&] {
        return "容器 #" + std::to_string(id) + "；目录 " + call_text(relative_directory) +
               "；名称 " + call_text(name);
      },
      [&] { impl_->core.create_container_directory(id, relative_directory, name); });
}

void CardLoginMountSession::move_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  traced_call("移动条目",
      [&] {
        return "来源容器 #" + std::to_string(source_id) + "；来源目录 " +
               call_text(source_relative_directory) + "；名称 " + call_text(name) +
               "；目标容器 #" + std::to_string(destination_id) + "；目标目录 " +
               call_text(destination_relative_directory);
      },
      [&] {
        impl_->core.move_container_entry(source_id, source_relative_directory, name,
                                         destination_id, destination_relative_directory);
      });
}

void CardLoginMountSession::copy_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  traced_call("复制条目",
      [&] {
        return "来源容器 #" + std::to_string(source_id) + "；来源目录 " +
               call_text(source_relative_directory) + "；名称 " + call_text(name) +
               "；目标容器 #" + std::to_string(destination_id) + "；目标目录 " +
               call_text(destination_relative_directory);
      },
      [&] {
        impl_->core.copy_container_entry(source_id, source_relative_directory, name,
                                         destination_id, destination_relative_directory);
      });
}

void CardLoginMountSession::export_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name,
    const std::wstring_view absolute_destination_directory) {
  traced_call("导出条目",
      [&] {
        return "容器 #" + std::to_string(id) + "；目录 " + call_text(relative_directory) +
               "；名称 " + call_text(name) + "；目标目录 " +
               call_text(absolute_destination_directory);
      },
      [&] {
        impl_->core.export_container_entry(id, relative_directory, name,
                                           absolute_destination_directory);
      });
}

void CardLoginMountSession::export_container_file(
    const std::uint32_t id,
    const std::wstring_view absolute_destination_directory) {
  traced_call("导出容器文件",
      [&] {
        return "容器 #" + std::to_string(id) + "；目标目录 " +
               call_text(absolute_destination_directory);
      },
      [&] { impl_->core.export_container_file(id, absolute_destination_directory); });
}

std::vector<ContainerStatus> CardLoginMountSession::container_statuses() {
  return impl_->core.container_statuses();
}

std::vector<ContainerOperationTarget> CardLoginMountSession::container_operation_targets() {
  return impl_->core.container_operation_targets();
}

ContainerAStatus CardLoginMountSession::container_a_status() {
  return impl_->core.container_a_status();
}

std::vector<OperationEvent> CardLoginMountSession::operation_history() {
  return impl_->core.operation_history();
}

void CardLoginMountSession::clear_operation_history() noexcept {
  impl_->core.clear_operation_history();
  if constexpr (USB_VAULT_DEBUG_LOG) note_call("清空操作记录", "完成", std::string());
}

bool CardLoginMountSession::operation_logging_enabled() {
  return impl_->core.operation_logging_enabled();
}
bool CardLoginMountSession::force_unmount_enabled() { return impl_->core.force_unmount_enabled(); }
bool CardLoginMountSession::auto_mount_enabled() { return impl_->core.auto_mount_enabled(); }
void CardLoginMountSession::set_auto_mount_enabled(bool enabled) {
  traced_call("设置登录后自动挂载", [&] { return std::string(enabled ? "开" : "关"); },
              [&] { impl_->core.set_auto_mount_enabled(enabled); });
}
void CardLoginMountSession::set_force_unmount_enabled(bool enabled) {
  traced_call("设置强制卸载偏好", [&] { return std::string(enabled ? "开" : "关"); },
              [&] { impl_->core.set_force_unmount_enabled(enabled); });
}
void CardLoginMountSession::reorder_containers(std::span<const std::uint32_t> ids) {
  traced_call("调整容器顺序", [&] { return "容器数 " + std::to_string(ids.size()); },
              [&] { impl_->core.reorder_containers(ids); });
}

void CardLoginMountSession::set_operation_logging_enabled(const bool enabled) {
  traced_call("设置操作记录", [&] { return std::string(enabled ? "开" : "关"); },
              [&] { impl_->core.set_operation_logging_enabled(enabled); });
}

void CardLoginMountSession::release_session_secrets() noexcept {
  impl_->core.release_session_secrets();
}

void CardLoginMountSession::unmount_active() {
  traced_call("全部卸载", [] { return std::string(); }, [&] { impl_->core.unmount_active(); });
}

bool CardLoginMountSession::force_unmount_active() {
  return traced_call("拔卡清理", [] { return std::string(); },
                     [&] { return impl_->core.force_unmount_active(); });
}

bool CardLoginMountSession::mounted() const noexcept {
  return impl_->core.mounted();
}

void CardLoginMountSession::force_unmount_container(const std::uint32_t id) {
  traced_call("强制卸载容器", [&] { return "容器 #" + std::to_string(id); },
              [&] { impl_->core.force_unmount_container(id); });
}

void CardLoginMountSession::force_unmount_user_active() {
  traced_call("强制卸载全部容器", [] { return std::string(); },
              [&] { impl_->core.force_unmount_user_active(); });
}

namespace detail {
namespace {

// SecureBuffer is deliberately not copyable, so every retained secret is
// duplicated explicitly and cleared on its own path.
[[nodiscard]] security::SecureBuffer duplicate_secret(
    const security::SecureBuffer& source) {
  security::SecureBuffer result(source.size());
  for (std::size_t index = 0; index < source.size(); ++index) {
    result.bytes()[index] = source.bytes()[index];
  }
  return result;
}

[[nodiscard]] security::SecureBuffer container_a_password_input() {
  security::SecureBuffer input(kContainerAPasswordInput.size());
  std::ranges::copy(kContainerAPasswordInput, input.bytes().begin());
  return input;
}

[[nodiscard]] std::wstring staging_path_for(
    const std::wstring_view container_path) {
  std::wstring path(container_path);
  path += L".preparing";
  return path;
}

[[nodiscard]] std::wstring file_name_of(
    const std::wstring_view container_path) {
  const std::size_t separator = container_path.find_last_of(L"\\/");
  const std::wstring_view name =
      separator == std::wstring_view::npos
          ? container_path
          : container_path.substr(separator + 1);
  return std::wstring(name);
}

// ---- Test-phase debug log -------------------------------------------------
// Every stage and every failure of the session is written to the runtime log.
// The detail strings are built only when the switch is on, and a failure to
// build or write one never changes what the session does. Passwords and keys
// are never part of a detail.
#define SESSION_TRACE(event, detail) USB_VAULT_TRACE("会话", (event), (detail))
// Inside a catch block only: adds the exception being handled.
#define SESSION_TRACE_FAILURE(event, detail) USB_VAULT_TRACE_FAILURE("会话", (event), (detail))

[[nodiscard]] std::string wide_text(const std::wstring_view text) {
  return diagnostics::utf8(text);
}
[[nodiscard]] std::string yes_no(const bool value) { return value ? "是" : "否"; }
[[nodiscard]] std::string drive_text(const wchar_t drive) {
  if (drive < L'A' || drive > L'Z') return "无";
  return std::string(1, static_cast<char>(drive)) + ":";
}
[[nodiscard]] std::string bytes_text(const std::uint64_t bytes) {
  char text[80]{};
  std::snprintf(text, sizeof(text), "%llu 字节（%.2f MiB）",
                static_cast<unsigned long long>(bytes),
                static_cast<double>(bytes) / 1048576.0);
  return text;
}
[[nodiscard]] std::string hex_text(const std::array<std::uint8_t, 16>& value) {
  char text[40]{};
  for (std::size_t index = 0; index < value.size(); ++index)
    std::snprintf(text + index * 2, 3, "%02X", value[index]);
  return text;
}
[[nodiscard]] std::string partition_text(const volume::PartitionIdentity& identity) {
  return "设备 " + hex_text(identity.device_id) + "；设备标志 " +
         std::to_string(identity.device_id_flags) + "；布局 " +
         std::to_string(identity.layout) + "；扇区 " +
         std::to_string(identity.logical_sector_bytes) + "；偏移 " +
         bytes_text(identity.offset_bytes) + "；长度 " +
         bytes_text(identity.length_bytes) + "；分区 GUID " +
         hex_text(identity.partition_id);
}
[[nodiscard]] std::string candidate_text(const volume::PartitionCandidate& target) {
  return "卷路径 " + wide_text(target.volume_path) + "；磁盘 " +
         std::to_string(target.disk_number) + "；分区 " +
         std::to_string(target.partition_number) + "；偏移 " +
         bytes_text(target.offset_bytes) + "；长度 " +
         bytes_text(target.length_bytes) + "；扇区 " +
         std::to_string(target.logical_sector_bytes) + "；布局 " +
         std::to_string(static_cast<int>(target.layout)) + "；可用性 " +
         std::to_string(static_cast<int>(target.availability)) + "；查询错误 " +
         std::to_string(target.query_error) + "；文件系统序列号 " +
         std::to_string(target.filesystem_serial) + "；设备 " +
         hex_text(target.device_id) + "；设备标志 " +
         std::to_string(target.device_id_flags) + "；分区 GUID " +
         hex_text(target.partition_id);
}
[[nodiscard]] std::string options_text(const volume::MountOptions& options) {
  return "请求盘符 " + drive_text(options.drive_letter) + "；PRF " +
         std::to_string(static_cast<int>(options.prf)) + "；PIM " +
         std::to_string(options.pim) + "；只读 " + yes_no(options.read_only) +
         "；通知资源管理器 " + yes_no(options.notify_shell) + "；隐藏卷保护 " +
         yes_no(options.hidden_volume_protection.has_value());
}
[[nodiscard]] std::string mount_result_text(const volume::MountResult& mounted) {
  return "驱动回报盘符 " + drive_text(mounted.drive_letter) + "；文件系统需检查 " +
         yes_no(mounted.filesystem_dirty) + "；因拒绝访问改只读 " +
         yes_no(mounted.read_only_after_access_denied) + "；因写保护改只读 " +
         yes_no(mounted.read_only_after_device_write_protected) + "；NTFS " +
         yes_no(mounted.is_ntfs) + "；主密钥存在弱点 " +
         yes_no(mounted.master_key_vulnerable) + "；耗时 " +
         std::to_string(mounted.elapsed.count()) + " 毫秒";
}
[[nodiscard]] std::string unmount_result_text(const volume::UnmountResult& result) {
  return "尝试次数 " + std::to_string(result.attempts) + "；隐藏卷保护已触发 " +
         yes_no(result.hidden_volume_protection_triggered) + "；耗时 " +
         std::to_string(result.elapsed.count()) + " 毫秒";
}
[[nodiscard]] std::string container_text(const SessionContainer& container) {
  return "容器 #" + std::to_string(container.id) + "；名称 " +
         wide_text(container.name) + "；" +
         (container.partition ? "分区 " + partition_text(*container.partition)
                              : "文件 " + wide_text(container.path)) +
         "；已挂载 " + yes_no(container.mounted) + "；盘符 " +
         drive_text(container.drive_letter) + "；登录派生 " +
         yes_no(container.release_first) + "；派生已确定 " +
         yes_no(container.derivation_known) + "；维护中 " +
         yes_no(container.maintenance) + "；维护需恢复 " +
         yes_no(container.maintenance_recovery_required) + "；转换状态 " +
         std::to_string(container.conversion_state);
}

// A cancel that lands while the helper is finishing still means no container.
// Everything written so far is under the staging name, so throwing here hands
// the file to the same cleanup a failure uses: asking to cancel always ends
// with nothing published, whichever side won the race.
void throw_if_creation_cancelled(
    const CardLoginMountSessionRuntime& runtime) {
  if (runtime.creation_cancelled && runtime.creation_cancelled()) {
    throw std::system_error(
        volume_creation::make_error_code(
            volume_creation::CreationError::operation_cancelled),
        "container creation was cancelled");
  }
}

[[noreturn]] void cleanup_incomplete_container(
    CardLoginMountSessionRuntime& runtime,
    const std::wstring_view staging_path,
    const std::exception_ptr& operation_failure) {
  SESSION_TRACE("清理未完成的暂存文件", "暂存路径 " + wide_text(staging_path));
  try {
    runtime.remove_incomplete(staging_path);
  } catch (...) {
    SESSION_TRACE_FAILURE("清理暂存文件失败", "暂存路径 " + wide_text(staging_path));
    throw ContainerCreationCleanupError(operation_failure,
                                        std::current_exception());
  }
  SESSION_TRACE("暂存文件已清理", "暂存路径 " + wide_text(staging_path));
  std::rethrow_exception(operation_failure);
}

// Drive letters are compared with this and nothing here wants a locale
// involved in that. The volume layer has its own copy behind a private header;
// duplicating four lines is cheaper than reaching across that boundary.
[[nodiscard]] wchar_t upper_drive_letter(const wchar_t letter) noexcept {
  return (letter >= L'a' && letter <= L'z')
             ? static_cast<wchar_t>(letter - L'a' + L'A')
             : letter;
}

[[nodiscard]] SessionContainer* find_by_id(
    CardLoginMountSessionState& state, const std::uint32_t id) noexcept {
  for (SessionContainer& container : state.containers) {
    if (container.id == id) return &container;
  }
  return nullptr;
}

// A successful provider result always identifies the mounted driver device.
// Keep the range check as a runtime-contract guard; production completion
// must not return zero after a successful mount or query inventory afterward.
[[nodiscard]] wchar_t settled_drive_letter(const volume::MountResult& mounted) {
  if (mounted.drive_letter < L'A' || mounted.drive_letter > L'Z') {
    throw std::runtime_error(
        "the driver did not report which drive the volume mounted on");
  }
  return mounted.drive_letter;
}

[[nodiscard]] SessionContainer* find_by_path(
    CardLoginMountSessionState& state,
    const std::wstring_view path) noexcept {
  for (SessionContainer& container : state.containers) {
    if (!container.partition && container.path == path) return &container;
  }
  return nullptr;
}

// Adoption is idempotent so a login mount, a residual-mount record and an
// explicit mount all converge on one entry per container path.
SessionContainer& adopt(CardLoginMountSessionState& state,
                        const std::wstring_view path,
                        const wchar_t drive_letter, const bool mounted,
                        const bool release_first = false,
                        const bool derivation_known = true,
                        const bool newest = false) {
  if (SessionContainer* const existing = find_by_path(state, path)) {
    existing->drive_letter = drive_letter;
    existing->mounted = mounted;
    existing->release_first = existing->release_first || release_first;
    existing->derivation_known =
        existing->derivation_known || derivation_known;
    return *existing;
  }
  state.containers.push_back({.id = state.next_container_id,
                              .path = std::wstring(path),
                              .name = file_name_of(path),
                              .drive_letter = drive_letter,
                              .mounted = mounted,
                              .release_first = release_first,
                              .derivation_known = derivation_known});
  ++state.next_container_id;
  if (newest) {
    std::rotate(state.containers.begin(), state.containers.end() - 1, state.containers.end());
    return state.containers.front();
  }
  return state.containers.back();
}

// The published file is already a container. Keep it in the session before the
// mount is attempted, so a later mount failure still has a row to show.
[[nodiscard]] std::uint32_t retain_published_container(
    CardLoginMountSessionState& state, const std::wstring_view path,
    const bool release_first) {
  return adopt(state, path, wchar_t{}, false, release_first, true, true).id;
}

void record_created_mount(CardLoginMountSessionState& state,
                          const std::uint32_t id, const wchar_t drive_letter) {
  SessionContainer* const entry = find_by_id(state, id);
  if (entry == nullptr) {
    throw std::logic_error("created container left the session");
  }
  entry->drive_letter = drive_letter;
  entry->mounted = true;
}

// What a volume that has left its drive no longer has. Anything that differs
// by path -- ending imports, measurements or logging -- stays with the caller.
void mark_released(SessionContainer& container) noexcept {
  container.mounted = false;
  container.maintenance = false;
  container.maintenance_identity.reset();
  container.drive_letter = wchar_t{};
}

SessionContainer& adopt_partition(CardLoginMountSessionState& state,
    const volume::PartitionIdentity& identity, const std::uint32_t sequence) {
  if (!volume::valid_partition_identity(identity)) throw std::invalid_argument("invalid partition identity");
  for (auto& entry : state.containers) if (entry.partition == identity) return entry;
  if (state.next_container_id == UINT32_MAX) throw std::overflow_error("container IDs exhausted");
  SessionContainer entry;
  entry.id = state.next_container_id++;
  // The recorded creation sequence, so the name is the container's own and
  // does not move when the list is rearranged. A record with none left to read
  // falls back to a session-local count, which is what every record had
  // before the sequence was written down.
  const auto shown = sequence ? sequence : state.next_partition_ordinal;
  entry.name = L"磁盘容器 " + std::to_wstring(shown);
  // The fallback stays one past the highest number handed out, so a record
  // with no sequence never lands on one a recorded container answers to.
  state.next_partition_ordinal = std::max(state.next_partition_ordinal, shown + 1);
  entry.derivation_known = false;
  entry.partition = identity;
  state.containers.push_back(std::move(entry));
  // Newest first, the way a newly created file container is placed. The
  // index records the same position, so it survives the session.
  std::rotate(state.containers.begin(), state.containers.end() - 1,
              state.containers.end());
  return state.containers.front();
}

}  // namespace

CardLoginMountSessionCore::CardLoginMountSessionCore(
    CardAuthenticationSession& authentication,
    CardLoginMountSessionRuntime& runtime,
    ContainerLayout layout)
    : authentication_(authentication),
      runtime_(runtime),
      layout_(std::move(layout)) {
  state_.operation_logging_enabled = runtime_.operation_logging_enabled();
}

void CardLoginMountSessionCore::record_operation(
    const OperationKind kind,
    const OperationOutcome outcome,
    const std::wstring_view container_name,
    const wchar_t drive_letter) noexcept {
  if (!state_.operation_logging_enabled) return;
  try {
    const auto completed =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
    OperationEvent event{
        .sequence = state_.next_operation_sequence,
        .completed_at_unix_ms = completed.count(),
        .kind = kind,
        .outcome = outcome,
        .container_name = std::wstring(container_name),
        .drive_letter = drive_letter,
    };

    const bool full = state_.operation_count == state_.operation_events.size();
    const std::size_t slot = (state_.operation_start + state_.operation_count) %
                             state_.operation_events.size();
    const std::size_t destination = full ? state_.operation_start : slot;
    state_.operation_events[destination] = std::move(event);
    if (full) {
      state_.operation_start =
          (state_.operation_start + 1) % state_.operation_events.size();
    } else {
      ++state_.operation_count;
    }
    ++state_.next_operation_sequence;
  } catch (...) {
    USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    // History is informational. Allocation or clock failure must never change
    // the result of the security or volume operation being recorded.
  }
}

std::vector<OperationEvent> CardLoginMountSessionCore::operation_history() {
  std::vector<OperationEvent> result;
  result.reserve(state_.operation_count);
  for (std::size_t index = 0; index < state_.operation_count; ++index) {
    const std::size_t slot =
        (state_.operation_start + index) % state_.operation_events.size();
    result.push_back(state_.operation_events[slot]);
  }
  return result;
}

void CardLoginMountSessionCore::clear_operation_history() noexcept {
  for (OperationEvent& event : state_.operation_events) {
    event.container_name.clear();
    event.sequence = 0;
    event.completed_at_unix_ms = 0;
    event.kind = OperationKind::session_opened;
    event.outcome = OperationOutcome::succeeded;
    event.drive_letter = wchar_t{};
  }
  state_.operation_start = 0;
  state_.operation_count = 0;
  state_.next_operation_sequence = 1;
}

void CardLoginMountSessionCore::consume_authenticated_login(
    security::SecureBuffer root_secret,
    security::SecureBuffer login_password_utf8) {
  if (state_.mounted() || authentication_.authenticated()) {
    root_secret.clear();
    login_password_utf8.clear();
    throw std::logic_error(
        "authenticated card mount session is already active");
  }

  try {
    authentication_.establish(std::move(root_secret));
    clear_operation_history();
    record_operation(OperationKind::session_opened,
                     OperationOutcome::succeeded);
    state_.hidden_container_password = security::SecureBuffer();
    // Retained for the card-record rewrite and for the shared hidden-volume
    // derivation. It is the password itself rather than a derivative because
    // rewrapping an envelope needs the password as key material, and the
    // record has no third slot to spare.
    state_.login_password = duplicate_secret(login_password_utf8);
    SESSION_TRACE("登录认证已建立", "登录口令长度 " +
                                        std::to_string(login_password_utf8.size()) + " 字节");
    const bool layout_b = runtime_.container_exists(layout_.container_b.path);
    SESSION_TRACE("检查布局容器 B", "路径 " + wide_text(layout_.container_b.path) +
                                        "；存在 " + yes_no(layout_b));
    if (layout_b) {
      // The layout path predates user-chosen locations and is adopted only
      // when a file is really sitting there. Its volume password is derived on
      // first use like that of every other hidden-volume container.
      static_cast<void>(adopt(state_, layout_.container_b.path, wchar_t{},
                              false, true));
    }
    const bool layout_a = runtime_.container_exists(layout_.container_a.path);
    SESSION_TRACE("检查布局容器 A", "路径 " + wide_text(layout_.container_a.path) +
                                        "；存在 " + yes_no(layout_a));
    if (layout_a) {
      static_cast<void>(adopt(state_, layout_.container_a.path, wchar_t{},
                              false));
    }
    adopt_remembered_containers();
    // Login only authenticates and lists what exists. Mounting is always a
    // later, separate request; the page's automatic mount after login is one
    // of those, run as an ordinary batch (8.7.5).
    login_password_utf8.clear();
    SESSION_TRACE("登录完成", "收录容器数 " + std::to_string(state_.containers.size()));
    for (const SessionContainer& container : state_.containers)
      SESSION_TRACE("登录后容器", container_text(container));
  } catch (...) {
    SESSION_TRACE_FAILURE("登录会话建立失败", "");
    login_password_utf8.clear();
    throw;
  }
}

void CardLoginMountSessionCore::create_container_a(
    const std::uint64_t size_bytes) {
  static_cast<void>(create_container(layout_.container_a.path, size_bytes));
}

security::SecureBuffer CardLoginMountSessionCore::derive_standard_password() {
  security::SecureBuffer fixed_input = container_a_password_input();
  try {
    security::SecureBuffer volume_password =
        runtime_.derive_container_password(
            fixed_input, authentication_.root_secret_,
            layout_.container_a.salt);
    fixed_input.clear();
    SESSION_TRACE("标准派生完成", "输出长度 " + std::to_string(volume_password.size()) + " 字节");
    return volume_password;
  } catch (...) {
    SESSION_TRACE_FAILURE("标准派生失败", "");
    fixed_input.clear();
    throw;
  }
}

const security::SecureBuffer&
CardLoginMountSessionCore::hidden_container_password() {
  if (!state_.hidden_container_password.empty()) {
    return state_.hidden_container_password;
  }
  if (state_.login_password.empty()) {
    throw std::logic_error(
        "this session no longer holds the input it was opened with");
  }
  // One derivation opens every hidden-volume container: they share the salt
  // and the pair of passwords, so the login input is run through the KDF once
  // and the result serves the rest of the session.
  try {
    state_.hidden_container_password = runtime_.derive_container_password(
        state_.login_password, authentication_.root_secret_,
        layout_.container_b.salt);
  } catch (...) {
    SESSION_TRACE_FAILURE("登录派生失败", "");
    throw;
  }
  SESSION_TRACE("登录派生完成", "输出长度 " +
                                  std::to_string(state_.hidden_container_password.size()) + " 字节");
  return state_.hidden_container_password;
}

security::SecureBuffer CardLoginMountSessionCore::container_index_key() {
  return derive_shared_container_index_key_v1(
      authentication_.root_secret_.bytes());
}

void CardLoginMountSessionCore::remember_container_path(
    const std::wstring_view path) noexcept {
  // Every failure here is swallowed on purpose. The container file already
  // exists; refusing the whole operation because its path could not be written
  // down would throw away a finished container to protect a convenience. The
  // cost of the silence is that the path is simply not remembered, which the
  // session shows anyway for as long as it lasts.
  try {
    if (!runtime_.remember_container_paths()) {
      SESSION_TRACE("未记录容器路径：记住容器已关闭", "路径 " + wide_text(path));
      return;
    }
    security::SecureBuffer key = container_index_key();
    std::vector<std::byte> file = runtime_.read_container_index();
    const ContainerIndexWriteV1 written =
        prepend_container_index_path_v1(key.bytes(), file, path);
    key.clear();
    SESSION_TRACE("写入容器索引", "路径 " + wide_text(path) + "；结果 " +
                                      std::to_string(static_cast<int>(written)) +
                                      "；索引大小 " + std::to_string(file.size()) + " 字节");
    if (written == ContainerIndexWriteV1::added) {
      runtime_.write_container_index(file);
      SESSION_TRACE("容器索引已保存", "路径 " + wide_text(path));
    }
  } catch (...) {
    SESSION_TRACE_FAILURE("记录容器路径失败", "路径 " + wide_text(path));
  }
}

void CardLoginMountSessionCore::forget_container_path(
    const std::wstring_view path) noexcept {
  try {
    security::SecureBuffer key = container_index_key();
    std::vector<std::byte> file = runtime_.read_container_index();
    const bool removed =
        remove_container_index_path_v1(key.bytes(), file, path);
    key.clear();
    SESSION_TRACE("从容器索引移除路径", "路径 " + wide_text(path) + "；找到 " + yes_no(removed));
    if (removed) {
      runtime_.write_container_index(file);
    }
  } catch (...) {
    SESSION_TRACE_FAILURE("从容器索引移除路径失败", "路径 " + wide_text(path));
  }
}

void CardLoginMountSessionCore::reorder_containers(
    const std::span<const std::uint32_t> ids) {
  if (!authentication_.authenticated()) throw std::logic_error("ordering requires authentication");
  if (ids.empty() || ids.size() != state_.containers.size() || ids.size() > 1024)
    throw std::invalid_argument("ordering must contain every current container");
  std::vector<SessionContainer> ordered;
  // The user drags one list holding both kinds, so the saved order has to name
  // both. Recording only the paths left every disk container with no position
  // of its own, to be re-derived from its random slot on the next login.
  std::vector<ContainerIndexRef> refs;
  ordered.reserve(ids.size());
  refs.reserve(ids.size());
  for (const auto id : ids) {
    const auto* entry = find_by_id(state_, id);
    if (!entry || std::any_of(ordered.begin(), ordered.end(),
        [id](const auto& item) { return item.id == id; }))
      throw std::invalid_argument("invalid container order");
    ordered.push_back(*entry);
    refs.push_back(entry->partition
        ? ContainerIndexRef{.partition = *entry->partition}
        : ContainerIndexRef{.path = entry->path});
  }
  if (runtime_.remember_container_paths()) {
    auto key = container_index_key();
    auto file = runtime_.read_container_index();
    const auto remembered = list_container_index_entries_v1(key.bytes(), file);
    for (const auto& ref : refs) {
      if (ref.partition) continue;
      if (std::any_of(remembered.begin(), remembered.end(), [&](const auto& saved) {
            return container_index_ref_matches_v1(ref, saved.path, saved.partition);
          })) continue;
      if (add_container_index_path_v1(key.bytes(), file, ref.path) == ContainerIndexWriteV1::full)
        throw std::runtime_error("container index is full");
    }
    // A partition is recorded when it is created and its record goes when it is
    // forgotten, so the index knows every one the session holds. Ordering names
    // only what it can find rather than refusing the whole arrangement over a
    // record that is somehow not there.
    std::erase_if(refs, [&](const auto& ref) {
      return ref.partition && !std::any_of(remembered.begin(), remembered.end(), [&](const auto& saved) {
        return container_index_ref_matches_v1(ref, saved.path, saved.partition);
      });
    });
    reorder_container_index_v1(key.bytes(), file, refs);
    key.clear();
    runtime_.write_container_index(file);
    SESSION_TRACE("容器顺序已写入索引", "记录数 " + std::to_string(refs.size()));
  }
  state_.containers.swap(ordered);
  if constexpr (USB_VAULT_DEBUG_LOG) {
    try {
      std::string order;
      for (const auto id : ids) order += (order.empty() ? "#" : "，#") + std::to_string(id);
      SESSION_TRACE("容器顺序已更新", order);
    } catch (...) {
      USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    }
  }
}

bool CardLoginMountSessionCore::remembering_container_paths() {
  return runtime_.remember_container_paths();
}

void CardLoginMountSessionCore::set_remembering_container_paths(
    const bool remember) {
  // Turning it off stops new paths being written and stops the list coming
  // back; it does not go through the index deleting what is already there. A
  // switch that quietly destroyed records could not be undone by flipping it
  // again, and the user asked to stop remembering, not to forget.
  runtime_.set_remember_container_paths(remember);
  SESSION_TRACE("记住容器设置已更改", "开启 " + yes_no(remember));
}

void CardLoginMountSessionCore::adopt_remembered_containers() noexcept {
  try {
    if (!runtime_.remember_container_paths()) {
      SESSION_TRACE("未读取容器索引：记住容器已关闭", "");
      return;
    }
    security::SecureBuffer key = container_index_key();
    std::vector<std::byte> file = runtime_.read_container_index();
    SESSION_TRACE("读取容器索引", "索引大小 " + std::to_string(file.size()) + " 字节");
    // Records written before the creation sequence existed get one now, once.
    // A disk container's name depends on it, so settling it here is what stops
    // the name being recomputed -- and moving -- on every login.
    const auto prepared = prepare_container_index_v1(key.bytes(), file);
    SESSION_TRACE("解析容器索引", "记录数 " + std::to_string(prepared.entries.size()) +
                                      "；补写序号 " + yes_no(prepared.numbered));
    if (prepared.numbered) runtime_.write_container_index(file);
    std::vector<ContainerIndexRef> refs;
    refs.reserve(prepared.entries.size());
    for (const auto& entry : prepared.entries)
      refs.push_back({.path = entry.path, .partition = entry.partition});
    key.clear();
    // Adoption inserts at the front, so traverse backward to preserve saved
    // order. Display numbers are fixed record attributes, not creation ranks.
    for (auto item = prepared.entries.rbegin(); item != prepared.entries.rend(); ++item)
      if (item->partition) {
        adopt_partition(state_, *item->partition, item->sequence).conversion_state =
            static_cast<std::uint32_t>(item->conversion);
        SESSION_TRACE("从索引收录分区", partition_text(*item->partition) + "；序号 " +
                                            std::to_string(item->sequence) + "；转换状态 " +
                                            std::to_string(static_cast<int>(item->conversion)));
      }
    for (const auto& ref : refs) {
      if (ref.partition) continue;
      // A remembered path whose file is not here is passed over and left in
      // the index. A container on a drive that is not plugged in is absent in
      // exactly the way a deleted one is, and clearing a slot on that evidence
      // would throw the record away for good.
      if (!runtime_.container_exists(ref.path)) {
        SESSION_TRACE("索引中的容器文件不在，跳过", "路径 " + wide_text(ref.path));
        continue;
      }
      static_cast<void>(adopt(state_, ref.path, wchar_t{}, false, false, false));
      SESSION_TRACE("从索引收录文件容器", "路径 " + wide_text(ref.path));
    }
    // Layout containers may already be adopted. Match all visible entries to
    // the saved order without changing mount flags or dropping absent paths.
    // Both kinds are ranked against one list now, so a disk container keeps the
    // position it was given instead of following the slot it landed in.
    struct RankedPosition {
      std::size_t position;
      std::size_t rank;
    };
    std::vector<RankedPosition> positions;
    positions.reserve(state_.containers.size());
    for (std::size_t position = 0; position < state_.containers.size(); ++position) {
      const auto& entry = state_.containers[position];
      const auto found = std::find_if(refs.begin(), refs.end(), [&](const auto& ref) {
        return container_index_ref_matches_v1(ref, entry.path, entry.partition);
      });
      positions.push_back({position, static_cast<std::size_t>(found - refs.begin())});
    }
    // Unmatched entries share the last rank and retain their session order.
    std::stable_sort(positions.begin(), positions.end(),
        [](const auto& a, const auto& b) { return a.rank < b.rank; });
    // Copy before swapping: allocation failure must not leave live entries
    // moved from, including their mount ownership and maintenance state.
    std::vector<SessionContainer> ordered;
    ordered.reserve(positions.size());
    for (const auto& item : positions)
      ordered.push_back(state_.containers[item.position]);
    state_.containers.swap(ordered);
  } catch (...) {
    SESSION_TRACE_FAILURE("从容器索引恢复列表失败", "");
  }
}

CardLoginMountSessionCore::ProbedMount
CardLoginMountSessionCore::mount_with_either_derivation(
    const std::wstring_view path) {
  security::SecureBuffer standard_password;
  wchar_t drive_letter{};
  volume::MountOptions options{};
  try {
    standard_password = derive_standard_password();
    drive_letter = reserve_drive_letter();
    options = {.drive_letter = drive_letter,
               .prf = volume::Pkcs5Prf::sha512,
               .pim = 0,
               .read_only = false,
               .notify_shell = false};
    SESSION_TRACE("以标准派生尝试挂载", "路径 " + wide_text(path) + "；" + options_text(options));
    const volume::MountResult mounted =
        runtime_.mount_created(path, standard_password, options);
    standard_password.clear();
    SESSION_TRACE("标准派生挂载成功", "路径 " + wide_text(path) + "；" + mount_result_text(mounted));
    return {.drive_letter = settled_drive_letter(mounted),
            .hidden_volume = false};
  } catch (const std::system_error& error) {
    SESSION_TRACE_FAILURE("标准派生挂载失败", "路径 " + wide_text(path));
    standard_password.clear();
    // The user selects a file, never a container class, and the page must not
    // offer that choice either. A file the standard derivation cannot open is
    // either built from the login input or not a container of ours at all, and
    // the two stay indistinguishable until the second derivation has been
    // tried.
    if (error.code() != volume::make_error_code(
                            volume::DriverError::authentication_failed) ||
        state_.login_password.empty()) {
      throw;
    }
    SESSION_TRACE("标准派生被拒，改用登录派生", "路径 " + wide_text(path));
  } catch (...) {
    SESSION_TRACE_FAILURE("标准派生挂载失败", "路径 " + wide_text(path));
    standard_password.clear();
    throw;
  }
  try {
    SESSION_TRACE("以登录派生尝试挂载", "路径 " + wide_text(path) + "；" + options_text(options));
    const volume::MountResult mounted =
        runtime_.mount_created(path, hidden_container_password(), options);
    SESSION_TRACE("登录派生挂载成功", "路径 " + wide_text(path) + "；" + mount_result_text(mounted));
    return {.drive_letter = settled_drive_letter(mounted), .hidden_volume = true};
  } catch (...) {
    SESSION_TRACE_FAILURE("登录派生挂载失败", "路径 " + wide_text(path));
    throw;
  }
}

wchar_t CardLoginMountSessionCore::reserve_drive_letter() {
  wchar_t letter{};
  try {
    letter = runtime_.allocate_drive_letter();
  } catch (...) {
    SESSION_TRACE_FAILURE("分配盘符失败", "");
    throw;
  }
  SESSION_TRACE("分配盘符", "盘符 " + drive_text(letter) + "；原始值 " +
                                std::to_string(static_cast<unsigned int>(letter)));
  if (letter < L'A' || letter > L'Z') {
    throw std::runtime_error("allocated drive letter is out of range");
  }
  for (const SessionContainer& container : state_.containers) {
    if (container.mounted && container.drive_letter == letter) {
      SESSION_TRACE("分配到的盘符已被本会话占用", container_text(container));
      throw std::runtime_error(
          "allocated drive letter is already held by this session");
    }
  }
  return letter;
}

bool CardLoginMountSessionCore::begin_partition_conversion(const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  return convert_partition(target,std::nullopt,confirm);
}
bool CardLoginMountSessionCore::resume_partition_conversion(std::uint32_t id) {
  if (!authentication_.authenticated()) throw std::logic_error("resume requires authentication");
  const auto entry=find_by_id(state_,id);
  if (!entry || !entry->partition || entry->mounted || entry->conversion_state!=2)
    throw std::logic_error("only a normally paused partition can resume");
  const auto target=runtime_.resolve_conversion_target(*entry->partition);
  return convert_partition(target,id,[](const auto&){return true;});
}
bool CardLoginMountSessionCore::convert_partition(const volume::PartitionCandidate& target,
    std::optional<std::uint32_t> resume_id, const std::function<bool(const volume::PartitionCandidate&)>& confirm) {
  if (!authentication_.authenticated() || partition_format_active_ || !runtime_.remember_container_paths() ||
      !confirm || !runtime_.creation_cancelled || !runtime_.creation_revoked)
    throw std::logic_error("conversion requires an idle authenticated session and lifecycle controls");
  const auto snapshot=target;
  const volume::PartitionIdentity identity{target.device_id,target.device_id_flags,
      static_cast<std::uint32_t>(target.layout),target.logical_sector_bytes,target.offset_bytes,target.length_bytes,target.partition_id};
  if (!volume::valid_partition_identity(identity)) throw std::invalid_argument("invalid conversion identity");
  if (resume_id) {
    const auto entry=find_by_id(state_,*resume_id);
    if (!entry || entry->partition!=identity || entry->mounted || entry->conversion_state!=2)
      throw std::logic_error("resume identity changed");
  } else {
    for (const auto& entry:state_.containers) if(entry.partition==identity) throw std::logic_error("partition already recorded");
  }
  const auto generation=authentication_.generation_;
  const auto pause=runtime_.creation_cancelled;
  const auto revoked_query=runtime_.creation_revoked;
  const auto revoked=[&]{return !authentication_.authenticated() || authentication_.generation_!=generation || revoked_query();};
  const auto check=[&]{if(revoked())throw std::system_error(volume_creation::make_error_code(volume_creation::CreationError::operation_cancelled));};
  check();
  SESSION_TRACE("开始分区原地加密", candidate_text(target) + "；续作容器 " +
      (resume_id ? "#" + std::to_string(*resume_id) : std::string("无")));
  partition_format_active_=true;
  struct Reset {bool& value; ~Reset(){value=false;}} reset{partition_format_active_};
  auto password=derive_standard_password();
  if(password.size()!=security::kKdfV1OutputSize)throw std::logic_error("invalid conversion password");
  const auto id=resume_id ? *resume_id : adopt_partition(state_,identity,0).id;
  bool recorded=resume_id.has_value(), accepted=false, requested=false;
  auto persist=[&](PartitionConversionState status,bool insert){
    auto key=container_index_key(); auto file=runtime_.read_container_index();
    std::uint32_t numbered{};
    if(insert) {
      // Prepended, like an erase-created one: a record with no position sorts
      // by the slot it landed in, and that slot is chosen at random on purpose.
      if(prepend_container_index_partition(key.bytes(),file,identity,status,&numbered)!=ContainerIndexWriteV1::added)
        throw std::runtime_error("conversion record unavailable");
    } else {
      if(resume_id && status==PartitionConversionState::uncertain) {
        const auto entries=list_container_index_partition_entries(key.bytes(),file);
        const auto saved=std::ranges::find_if(entries,[&](const auto& entry){return entry.partition==identity;});
        if(saved==entries.end() || saved->conversion!=PartitionConversionState::paused)
          throw std::logic_error("durable resume permission changed");
      }
      if(!set_container_index_partition_state(key.bytes(),file,identity,status,&numbered))
        throw std::runtime_error("conversion record missing");
    }
    key.clear(); check(); runtime_.write_container_index(file);
    SESSION_TRACE("写入分区转换状态", "状态 " + std::to_string(static_cast<int>(status)) +
                                          "；新记录 " + yes_no(insert) + "；序号 " + std::to_string(numbered));
    auto entry=find_by_id(state_,id);
    if(!entry)throw std::logic_error("conversion session changed");
    entry->conversion_state=static_cast<std::uint32_t>(status);
    // The record now carries a number; the row was named before it existed.
    if(numbered)entry->name=L"磁盘容器 "+std::to_wstring(numbered);
  };
  try {
    const bool paused=runtime_.convert_partition(snapshot,!resume_id,password,[&]{
      if(requested || revoked())return false; requested=true;
      if(!confirm(snapshot) || revoked())return false;
      // Consume the paused permission durably BEFORE any new disk write.
      persist(PartitionConversionState::uncertain,!resume_id); recorded=true; accepted=true;
      return !revoked();
    },revoked,pause);
    SESSION_TRACE("分区原地加密返回", "已暂停 " + yes_no(paused) + "；已同意 " + yes_no(accepted));
    password.clear(); check();
    if(!accepted)throw std::logic_error("conversion returned without consent");
    persist(paused ? PartitionConversionState::paused : PartitionConversionState::normal,false);
    check();
    if(!paused && !pause()) {
      mount_adopted_container(id);
      if(revoked()) {unmount_container_impl(id,true); check();}
    }
    if(!paused && runtime_.creation_feedback)runtime_.creation_feedback(5,100,find_by_id(state_,id)->drive_letter);
    if (const auto* entry = find_by_id(state_, id)) SESSION_TRACE("分区原地加密结束", container_text(*entry));
    return paused;
  } catch (...) {
    SESSION_TRACE_FAILURE("分区原地加密失败", "已写入记录 " + yes_no(recorded) +
                                                "；已同意 " + yes_no(accepted) +
                                                "；已请求确认 " + yes_no(requested));
    if(!recorded)std::erase_if(state_.containers,[id](const auto& entry){return entry.id==id;});
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::create_partition(
    const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm,
    std::uint64_t hidden_bytes, const security::SecureBuffer* decoy) {
  if ((decoy == nullptr) != (hidden_bytes == 0) ||
      (decoy && (decoy->empty() || state_.login_password.empty() || hidden_bytes < 1024 * 1024 ||
       hidden_bytes % 65536 || hidden_bytes > target.length_bytes / 4 * 3)))
    throw std::invalid_argument("invalid partition pair inputs");
  if (!authentication_.authenticated()) throw std::logic_error("partition creation requires authentication");
  if (!runtime_.remember_container_paths()) throw std::logic_error("partition creation requires remembering containers");
  const volume::PartitionIdentity identity{target.device_id, target.device_id_flags,
      static_cast<std::uint32_t>(target.layout),
      target.logical_sector_bytes, target.offset_bytes, target.length_bytes, target.partition_id};
  if (!volume::valid_partition_identity(identity)) throw std::invalid_argument("invalid partition identity");
  for (const auto& entry : state_.containers)
    if (entry.partition == identity) throw std::logic_error("partition already recorded");
  auto key = container_index_key();
  auto index = runtime_.read_container_index();
  std::uint32_t sequence{};
  const auto insertion = prepend_container_index_partition(key.bytes(), index, identity,
      PartitionConversionState::normal, &sequence);
  SESSION_TRACE("开始创建分区容器", candidate_text(target) + "；隐藏卷 " +
      (decoy ? bytes_text(hidden_bytes) : std::string("无")) + "；索引写入结果 " +
      std::to_string(static_cast<int>(insertion)) + "；序号 " + std::to_string(sequence));
  if (insertion != ContainerIndexWriteV1::added) throw std::runtime_error("partition index unavailable or already recorded");
  key.clear();
  // Allocate the session row before irreversible work, but remove it on failure.
  const auto id = adopt_partition(state_, identity, sequence).id;
  bool recorded = false;
  try {
    format_partition(target, [&](const auto& frozen) {
      if (!confirm || !confirm(frozen)) return false;
      // Persist identity before any destructive write; failure refuses consent.
      // A later cancellation may leave an unmounted record, never a lost volume.
      runtime_.write_container_index(index);
      recorded = true;
      SESSION_TRACE("分区身份已写入索引", partition_text(identity));
      return true;
    }, hidden_bytes, decoy);
    if (decoy) {
      auto* entry = find_by_id(state_, id);
      entry->release_first = true;
      entry->derivation_known = true;
    }
    // The ciphertext is already on the partition, so from here the user can
    // no longer cancel: stage 4 closes the page's cancel gate, and a request
    // that won the race by a moment is ignored rather than reported as if the
    // write had never happened. Only a revoked card still stops the mount.
    if (runtime_.creation_feedback) {
      try {
        runtime_.creation_feedback(4, 0, 0);
      } catch (const std::system_error& error) {
        if (error.code() != volume_creation::make_error_code(
                volume_creation::CreationError::operation_cancelled)) throw;
        SESSION_TRACE("数据写完后收到的取消请求已忽略", partition_text(identity));
      }
    }
    const auto revoked = [this] {
      return runtime_.creation_revoked && runtime_.creation_revoked();
    };
    const auto created_unmounted = [] {
      return std::system_error(
          volume_creation::make_error_code(
              volume_creation::CreationError::mount_failed),
          "partition was created but not mounted");
    };
    if (revoked()) {
      SESSION_TRACE("分区写完后安全卡已撤销，不挂载", partition_text(identity));
      throw created_unmounted();
    }
    mount_adopted_container(id);
    if (revoked()) {
      // Removal can arrive inside the driver mount. Track the mount first so
      // failed cleanup remains visible to the deferred card-removal retry.
      SESSION_TRACE("挂载期间安全卡已撤销，卸下分区", partition_text(identity));
      unmount_container_impl(id, true);
      throw created_unmounted();
    }
    if (runtime_.creation_feedback) runtime_.creation_feedback(5, 100, find_by_id(state_, id)->drive_letter);
    if (const auto* entry = find_by_id(state_, id)) SESSION_TRACE("分区容器创建完成", container_text(*entry));
    return id;
  } catch (...) {
    SESSION_TRACE_FAILURE("创建分区容器失败", "索引记录已写入 " + yes_no(recorded) +
                                                "；" + partition_text(identity));
    if (!recorded) std::erase_if(state_.containers, [id](const auto& entry) { return entry.id == id; });
    throw;
  }
}

void CardLoginMountSessionCore::format_partition(
    const volume::PartitionCandidate& target,
    const std::function<bool(const volume::PartitionCandidate&)>& confirm_erase,
    std::uint64_t hidden_bytes, const security::SecureBuffer* decoy) {
  if (!authentication_.authenticated() || partition_format_active_) {
    throw std::logic_error("partition format requires an idle authenticated session");
  }
  if (!confirm_erase) throw std::invalid_argument("partition erase confirmation required");
  const volume::PartitionCandidate snapshot = target;
  const auto confirmation = confirm_erase;
  const auto cancellation = runtime_.creation_cancelled;
  if (!cancellation) throw std::invalid_argument("partition cancellation query required");
  const auto generation = authentication_.generation_;
  partition_format_active_ = true;
  struct Reset final {
    bool& active;
    ~Reset() { active = false; }
  } reset{partition_format_active_};
  // This session is serialized. Physical-card events reach the worker through
  // the captured atomic cancellation query, not concurrent secret mutation.
  const auto cancelled = [&] {
    return !authentication_.authenticated() ||
        authentication_.generation_ != generation ||
        (cancellation && cancellation());
  };
  const auto check = [&] {
    if (cancelled()) throw std::system_error(volume_creation::make_error_code(
        volume_creation::CreationError::operation_cancelled),
        "partition format was cancelled");
  };
  check();
  SESSION_TRACE("开始格式化分区", candidate_text(target) + "；隐藏卷 " +
      (decoy ? bytes_text(hidden_bytes) : std::string("无")));
  auto password = decoy ? runtime_.derive_container_password(*decoy, authentication_.root_secret_, layout_.container_b.salt) : derive_standard_password();
  if (password.size() != security::kKdfV1OutputSize) {
    throw std::logic_error("partition password derivation returned an invalid size");
  }
  check();
  bool requested = false;
  bool accepted = false;
  const auto confirm = [&] {
    if (requested || cancelled()) return false;
    requested = true;
    try {
      accepted = confirmation(snapshot) && !cancelled();
    } catch (...) {
      SESSION_TRACE_FAILURE("擦除确认回调失败", "");
      accepted = false;
    }
    SESSION_TRACE("擦除确认结果", "已同意 " + yes_no(accepted));
    return accepted;
  };
  try {
    if (decoy) {
      const auto& inner = hidden_container_password();
      if (inner.size() != security::kKdfV1OutputSize) throw std::logic_error("invalid partition inner key");
      runtime_.format_partition_pair(snapshot, hidden_bytes, password, inner, confirm, cancelled);
    } else runtime_.format_partition(snapshot, password, confirm, cancelled);
  } catch (...) {
    SESSION_TRACE_FAILURE("格式化分区失败", "已请求确认 " + yes_no(requested) +
                                              "；已同意 " + yes_no(accepted));
    throw;
  }
  SESSION_TRACE("格式化分区返回", "已同意 " + yes_no(accepted));
  password.clear();
  // A normal return means the ciphertext is on the partition. Checking the
  // cancel flag again would report a finished write as if it had been aborted.
  if (!accepted) throw std::logic_error("partition format completed without consent");
  // No file staging, removal, publication, index entry or mount is appropriate
  // here. A failure can leave partial ciphertext and must never trigger retry.
}

std::uint32_t CardLoginMountSessionCore::create_container(
    const std::wstring_view path, const std::uint64_t size_bytes) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "container creation requires an authenticated card session");
  }
  if (size_bytes == 0 || size_bytes % 512ULL != 0) {
    throw std::invalid_argument(
        "container size must be a non-zero multiple of 512 bytes");
  }
  if (find_by_path(state_, path) != nullptr) {
    throw std::logic_error("container path is already adopted");
  }

  const std::wstring staging_path = staging_path_for(path);
  if (runtime_.container_exists(path) ||
      runtime_.container_exists(staging_path)) {
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "container creation target already exists");
  }

  const std::wstring container_name = file_name_of(path);
  SESSION_TRACE("开始创建文件容器", "路径 " + wide_text(path) + "；暂存路径 " +
                                        wide_text(staging_path) + "；容量 " + bytes_text(size_bytes));
  security::SecureBuffer volume_password;
  bool staging_owned = false;
  bool published = false;
  bool listed = false;
  bool mounted = false;
  try {
    volume_password = derive_standard_password();
    runtime_.create_standard(staging_path, size_bytes, volume_password);
    staging_owned = true;
    SESSION_TRACE("暂存容器已生成", "暂存路径 " + wide_text(staging_path));
    throw_if_creation_cancelled(runtime_);
    if (runtime_.creation_feedback && runtime_.container_size_if_present(staging_path) == size_bytes) runtime_.creation_feedback(2, 100, 0);
    if (runtime_.creation_feedback) runtime_.creation_feedback(4, 0, 0);
    runtime_.publish_created(staging_path, path);
    published = true;
    SESSION_TRACE("容器已发布", "路径 " + wide_text(path));
    record_operation(OperationKind::container_created,
                     OperationOutcome::succeeded,
                     container_name);
    // Publication is the point the file becomes a container. Remember it
    // before mounting, or a mount failure leaves a real file off the list.
    const std::uint32_t id = retain_published_container(state_, path, false);
    listed = true;
    SESSION_TRACE("容器已收录", "容器 #" + std::to_string(id));
    remember_container_path(path);

    const volume::MountOptions options{.drive_letter = reserve_drive_letter(),
                                       .prf = volume::Pkcs5Prf::sha512,
                                       .pim = 0,
                                       .read_only = false,
                                       .notify_shell = false};
    SESSION_TRACE("挂载新建容器", "路径 " + wide_text(path) + "；" + options_text(options));
    const volume::MountResult mount_result =
        runtime_.mount_created(path, volume_password, options);
    SESSION_TRACE("新建容器挂载返回", mount_result_text(mount_result));
    const wchar_t drive_letter = settled_drive_letter(mount_result);
    record_created_mount(state_, id, drive_letter);
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    if (runtime_.creation_feedback) runtime_.creation_feedback(5, 100, drive_letter);
    volume_password.clear();
    runtime_.notify_mounted(drive_letter);
    if (const auto* entry = find_by_id(state_, id)) SESSION_TRACE("文件容器创建完成", container_text(*entry));
    return id;
  } catch (...) {
    SESSION_TRACE_FAILURE("创建文件容器失败", "路径 " + wide_text(path) + "；暂存已生成 " +
                                                yes_no(staging_owned) + "；已发布 " + yes_no(published) +
                                                "；已收录 " + yes_no(listed) + "；已挂载 " + yes_no(mounted));
    volume_password.clear();
    if (!published) {
      record_operation(OperationKind::container_created,
                       OperationOutcome::failed,
                       container_name);
    } else if (listed && !mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    if (staging_owned && !published) {
      cleanup_incomplete_container(runtime_, staging_path,
                                   std::current_exception());
    }
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::create_container_b(
    const std::wstring_view path,
    const std::uint64_t outer_size_bytes,
    const std::uint64_t hidden_size_bytes,
    const security::SecureBuffer& decoy_password_utf8) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "container creation requires an authenticated card session");
  }
  if (state_.login_password.empty()) {
    throw std::logic_error(
        "this session no longer holds the input it was opened with");
  }
  // A zero-length hidden volume is refused here and not only in the UI layer
  // that already refuses it: this entry point is the application contract, and
  // every other size invariant of the pair is stated in this same condition.
  if (outer_size_bytes == 0 || hidden_size_bytes == 0 ||
      outer_size_bytes % 512ULL != 0 ||
      hidden_size_bytes % 512ULL != 0 ||
      hidden_size_bytes >= outer_size_bytes) {
    throw std::invalid_argument(
        "outer and hidden sizes must be non-zero, aligned, and leave room for "
        "the outer volume");
  }
  if (decoy_password_utf8.size() == 0) {
    throw std::invalid_argument(
        "the decoy volume password is required in clear text");
  }

  if (find_by_path(state_, path) != nullptr) {
    throw std::logic_error("container path is already adopted");
  }
  const std::wstring staging_path = staging_path_for(path);
  if (runtime_.container_exists(path) ||
      runtime_.container_exists(staging_path)) {
    throw std::system_error(std::make_error_code(std::errc::file_exists),
                            "container creation target already exists");
  }
  const std::wstring container_name = file_name_of(path);
  SESSION_TRACE("开始创建含隐藏卷的文件容器", "路径 " + wide_text(path) + "；暂存路径 " +
                                                  wide_text(staging_path) + "；外层容量 " +
                                                  bytes_text(outer_size_bytes) + "；隐藏卷容量 " +
                                                  bytes_text(hidden_size_bytes) + "；诱饵口令长度 " +
                                                  std::to_string(decoy_password_utf8.size()) + " 字节");

  // The password the caller is logged in under opens the hidden volume; the
  // password set here opens the outer one. The session never decides which of
  // the two the caller "really" uses. Both derivations are shared by every
  // container of this kind, so a second one costs no extra key material.
  security::SecureBuffer outer_password;
  bool staging_owned = false;
  bool published = false;
  bool listed = false;
  bool mounted = false;
  try {
    outer_password =
        runtime_.derive_container_password(decoy_password_utf8,
                                           authentication_.root_secret_,
                                           layout_.container_b.salt);
    SESSION_TRACE("外层卷派生完成", "输出长度 " + std::to_string(outer_password.size()) + " 字节");
    const security::SecureBuffer& hidden_password =
        hidden_container_password();
    runtime_.create_with_hidden(staging_path,
                                outer_size_bytes,
                                hidden_size_bytes,
                                outer_password,
                                hidden_password);
    staging_owned = true;
    SESSION_TRACE("暂存容器已生成", "暂存路径 " + wide_text(staging_path));
    throw_if_creation_cancelled(runtime_);
    if (runtime_.creation_feedback && runtime_.container_size_if_present(staging_path) == outer_size_bytes) runtime_.creation_feedback(2, 100, 0);
    if (runtime_.creation_feedback) runtime_.creation_feedback(4, 0, 0);
    runtime_.publish_created(staging_path, path);
    published = true;
    SESSION_TRACE("容器已发布", "路径 " + wide_text(path));
    record_operation(OperationKind::container_created,
                     OperationOutcome::succeeded,
                     container_name);
    // Same rule as a standard container: the file is kept even if mounting it
    // fails. release_first stays set so a later mount still opens the hidden
    // volume with the login derivation.
    const std::uint32_t id = retain_published_container(state_, path, true);
    listed = true;
    SESSION_TRACE("容器已收录", "容器 #" + std::to_string(id));
    remember_container_path(path);

    const volume::MountOptions options{.drive_letter = reserve_drive_letter(),
                                       .prf = volume::Pkcs5Prf::sha512,
                                       .pim = 0,
                                       .read_only = false,
                                       .notify_shell = false};
    SESSION_TRACE("以登录派生挂载新建容器", "路径 " + wide_text(path) + "；" + options_text(options));
    const volume::MountResult mount_result =
        runtime_.mount_created(path, hidden_password, options);
    SESSION_TRACE("新建容器挂载返回", mount_result_text(mount_result));
    const wchar_t drive_letter = settled_drive_letter(mount_result);
    record_created_mount(state_, id, drive_letter);
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    if (runtime_.creation_feedback) runtime_.creation_feedback(5, 100, drive_letter);
    outer_password.clear();
    runtime_.notify_mounted(drive_letter);
    if (const auto* entry = find_by_id(state_, id)) SESSION_TRACE("含隐藏卷的文件容器创建完成", container_text(*entry));
    return id;
  } catch (...) {
    SESSION_TRACE_FAILURE("创建含隐藏卷的文件容器失败", "路径 " + wide_text(path) + "；暂存已生成 " +
                                                        yes_no(staging_owned) + "；已发布 " + yes_no(published) +
                                                        "；已收录 " + yes_no(listed) + "；已挂载 " + yes_no(mounted));
    outer_password.clear();
    if (!published) {
      record_operation(OperationKind::container_created,
                       OperationOutcome::failed,
                       container_name);
    } else if (listed && !mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    if (staging_owned && !published) {
      cleanup_incomplete_container(runtime_, staging_path,
                                   std::current_exception());
    }
    throw;
  }
}

SessionContainer& CardLoginMountSessionCore::maintenance_target(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  if (state_.login_password.empty()) {
    throw std::logic_error(
        "this session no longer holds the input it was opened with");
  }
  if (decoy_password_utf8.empty()) {
    throw std::invalid_argument(
        "the decoy volume password is required in clear text");
  }
  SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  return *entry;
}

wchar_t CardLoginMountSessionCore::mount_outer_for_maintenance(
    SessionContainer& entry,
    const security::SecureBuffer& outer_password,
    const MaintenanceMount mode) {
  // One file cannot serve two volumes, so whatever it serves now goes first.
  // Force is this flow's standing answer, agreed to before any of it starts.
  if (entry.mounted) unmount_container_impl(entry.id, true);

  // The hidden volume's password is the session's own, never the caller's: the
  // driver reads that header only to learn what to refuse writes to.
  const security::SecureBuffer& hidden_password = hidden_container_password();
  const auto refuse = [&]() {
    if (mode == MaintenanceMount::open) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       entry.name);
    }
    return DecoyMaintenanceRefused();
  };

  // Everything below -- the volume-type gate, the identity check and the
  // forced release when either refuses -- addresses this drive. It has to be
  // the one the driver reported, or a wrong-type mount would be left behind
  // while a letter that holds nothing is the one asked to let go of it.
  wchar_t drive_letter{};
  const char* const mode_text = mode == MaintenanceMount::open ? "打开维护" : "试挂";
  try {
    const volume::MountOptions options{.drive_letter = reserve_drive_letter(),
         .prf = volume::Pkcs5Prf::sha512,
         .pim = 0,
         .read_only = false,
         .notify_shell = false,
         .hidden_volume_protection =
             volume::HiddenVolumeProtection{.password = &hidden_password}};
    SESSION_TRACE(std::string("带保护挂载外层卷（") + mode_text + "）",
                  container_text(entry) + "；" + options_text(options));
    const volume::MountResult mount_result = entry.partition
        ? runtime_.mount_partition(*entry.partition, outer_password, options)
        : runtime_.mount_created(entry.path, outer_password, options);
    SESSION_TRACE("带保护挂载返回", mount_result_text(mount_result));
    drive_letter = settled_drive_letter(mount_result);
  } catch (...) {
    // Every driver rejection means the same thing to everyone above.
    SESSION_TRACE_FAILURE(std::string("带保护挂载外层卷失败（") + mode_text + "）", container_text(entry));
    throw refuse();
  }

  // The driver reports the outer type only while protecting a hidden volume,
  // so anything else means the wrong volume came up -- the hidden one included.
  const std::optional<volume::VolumeType> type =
      runtime_.mounted_volume_type(drive_letter);
  SESSION_TRACE("维护挂载的卷类型", "盘符 " + drive_text(drive_letter) + "；类型 " +
      (type ? std::to_string(static_cast<int>(*type)) : std::string("无")));
  if (!type.has_value() || *type != volume::VolumeType::outer) {
    try {
      static_cast<void>(runtime_.unmount_forced(drive_letter));
      SESSION_TRACE("类型不符，已强制卸下", "盘符 " + drive_text(drive_letter));
    } catch (...) {
      SESSION_TRACE_FAILURE("类型不符后强制卸下失败", "盘符 " + drive_text(drive_letter));
      // Still this session's to release later, so it is recorded rather than
      // dropped. The derivation flags stay untouched: nothing was established.
      entry.mounted = true;
      entry.drive_letter = drive_letter;
      throw refuse();
    }
    throw refuse();
  }

  std::optional<volume::MountIdentity> identity;
  try {
    if (mode == MaintenanceMount::open) {
      identity = runtime_.mount_identity(drive_letter);
      if (!identity->protected_outer_writable || identity->drive_letter != drive_letter)
        throw std::runtime_error("maintenance mount identity unavailable");
    }
  } catch (...) {
    SESSION_TRACE_FAILURE("维护挂载身份无法确认", "盘符 " + drive_text(drive_letter));
    try { static_cast<void>(runtime_.unmount_forced(drive_letter)); }
    catch (...) {
      SESSION_TRACE_FAILURE("身份无法确认后强制卸下失败", "盘符 " + drive_text(drive_letter));
      entry.mounted = true; entry.drive_letter = drive_letter;
    }
    throw refuse();
  }
  entry.maintenance_identity = std::move(identity);
  entry.maintenance_recovery_required = false;

  // Proven, not stored: this container has an outer volume the supplied
  // password opens and a hidden one the session's own derivation opens.
  entry.mounted = true;
  entry.drive_letter = drive_letter;
  entry.release_first = true;
  entry.derivation_known = true;
  entry.maintenance = mode == MaintenanceMount::open;
  SESSION_TRACE(std::string("带保护挂载完成（") + mode_text + "）", container_text(entry));
  if (mode == MaintenanceMount::open) {
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     entry.name,
                     drive_letter);
    runtime_.notify_mounted(drive_letter);
  }
  return drive_letter;
}

wchar_t CardLoginMountSessionCore::open_decoy_maintenance(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  SessionContainer& entry = maintenance_target(id, decoy_password_utf8);
  if (entry.mounted && entry.maintenance) return entry.drive_letter;
  const bool switched = release_other_maintenance(id);
  security::SecureBuffer outer_password;
  try {
    outer_password = runtime_.derive_container_password(
        decoy_password_utf8, authentication_.root_secret_,
        layout_.container_b.salt);
    const wchar_t drive_letter = mount_outer_for_maintenance(
        entry, outer_password, MaintenanceMount::open);
    state_.decoy_outer_password = std::move(outer_password);
    return drive_letter;
  } catch (...) {
    SESSION_TRACE_FAILURE("打开诱饵维护失败", container_text(entry));
    outer_password.clear();
    if (switched) runtime_.end_measurements();
    throw;
  }
}

DecoyMaintenanceSurvey CardLoginMountSessionCore::survey_decoy_maintenance(
    const security::SecureBuffer& decoy_password_utf8,
    const SurveyProgress& progress,
    const SurveyCancellation& cancelled) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  if (state_.login_password.empty()) {
    throw std::logic_error(
        "this session no longer holds the input it was opened with");
  }
  if (decoy_password_utf8.empty()) {
    throw std::invalid_argument(
        "the decoy volume password is required in clear text");
  }

  // One derivation for the whole walk: every container of this kind shares the
  // salt, so paying per container would buy nothing.
  security::SecureBuffer outer_password = runtime_.derive_container_password(
      decoy_password_utf8, authentication_.root_secret_,
      layout_.container_b.salt);

  DecoyMaintenanceSurvey survey;
  const auto total = static_cast<std::uint32_t>(state_.containers.size());
  std::uint32_t done = 0;
  try {
    for (SessionContainer& entry : state_.containers) {
      if (cancelled && cancelled()) {
        survey.cancelled = true;
        break;
      }
      // A container known to open on the standard derivation has no outer
      // volume to find, so it is left exactly as the user is using it.
      if (!(entry.derivation_known && !entry.release_first)) {
        try {
          const wchar_t drive_letter = mount_outer_for_maintenance(
              entry, outer_password, MaintenanceMount::trial);
          static_cast<void>(runtime_.unmount_forced(drive_letter));
          mark_released(entry);
          survey.maintainable.push_back(entry.id);
          SESSION_TRACE("搜索：可维护", container_text(entry));
        } catch (const DecoyMaintenanceRefused&) {
          USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
          // Not one this password maintains, which is all anyone learns.
          SESSION_TRACE("搜索：不可维护", container_text(entry));
        } catch (...) {
          // A container that would not release, or a letter that could not be
          // taken: nothing was learned about it, and the walk goes on.
          SESSION_TRACE_FAILURE("搜索：无法判断", container_text(entry));
          ++survey.unreachable;
        }
      } else {
        SESSION_TRACE("搜索：跳过标准派生容器", container_text(entry));
      }
      ++done;
      if (progress) progress(done, total);
    }
  } catch (...) {
    SESSION_TRACE_FAILURE("诱饵搜索失败", "");
    outer_password.clear();
    throw;
  }
  SESSION_TRACE("诱饵搜索结束", "可维护 " + std::to_string(survey.maintainable.size()) +
                                    "；无法判断 " + std::to_string(survey.unreachable) +
                                    "；已取消 " + yes_no(survey.cancelled) +
                                    "；总数 " + std::to_string(total));

  // Keeping it is what spares the next page a second prompt; keeping it after
  // finding nothing would only be holding a secret for no one.
  if (!survey.maintainable.empty()) {
    state_.decoy_outer_password = std::move(outer_password);
  } else {
    outer_password.clear();
  }
  return survey;
}

DecoyMaintenanceSpace CardLoginMountSessionCore::decoy_maintenance_space(
    const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  const SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  // Only a volume opened for maintenance is measured: measuring asks for
  // consent, and nothing else in the product has a reason to spend it.
  if (!entry->mounted || !entry->maintenance) {
    throw std::logic_error("this container is not open for maintenance");
  }
  if (!entry->maintenance_identity || state_.decoy_outer_password.empty())
    throw std::logic_error("authenticated maintenance binding unavailable");
  const auto verify_identity = [&] {
    try {
      if (runtime_.mount_identity(entry->drive_letter) != *entry->maintenance_identity)
        throw std::runtime_error("maintenance mount identity changed");
    } catch (...) {
      SESSION_TRACE_FAILURE("维护卷身份校验失败", container_text(*entry));
      runtime_.end_measurements();
      throw;
    }
  };
  if (entry->partition)
    throw std::logic_error("partition maintenance capacity measurement is not supported");
  verify_identity();
  const volume::VolumeSpace space = runtime_.volume_space(entry->drive_letter);
  const auto safe = runtime_.measure_protected_space(
      entry->path, *entry->maintenance_identity, state_.decoy_outer_password,
      hidden_container_password());
  verify_identity();
  SESSION_TRACE("维护卷空间", container_text(*entry) + "；总量 " + bytes_text(space.total_bytes) +
                                  "；剩余 " + bytes_text(space.free_bytes) + "；安全可写 " + bytes_text(safe));
  return {.total_bytes = space.total_bytes,
          .free_bytes = space.free_bytes,
          .safe_free_bytes = safe};
}

bool CardLoginMountSessionCore::decoy_maintenance_write_blocked(const std::uint32_t id) {
  if (!authentication_.authenticated()) throw std::logic_error("maintenance unavailable");
  const SessionContainer* const entry = find_by_id(state_, id);
  if (!entry || !entry->mounted || !entry->maintenance)
    throw std::logic_error("maintenance unavailable");
  const auto type = runtime_.mounted_volume_type(entry->drive_letter);
  if (type == volume::VolumeType::outer_write_prevented) return true;
  if (type == volume::VolumeType::outer) return false;
  // Missing or unexpected volumes are unknown, never evidence of protection.
  throw std::runtime_error("maintenance status unavailable");
}

void CardLoginMountSessionCore::end_decoy_maintenance(const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  const SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (!entry->mounted || !entry->maintenance) {
    throw std::logic_error("this container is not open for maintenance");
  }
  // The deliberate unmount, not the forced one: ending maintenance is not an
  // emergency, and a volume still in use is the user's to settle.
  SESSION_TRACE("结束诱饵维护", container_text(*entry));
  unmount_container_impl(id, false);
}

void CardLoginMountSessionCore::import_external_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view absolute_source,
    const volume::TransferCancellation& cancelled, const std::function<void(unsigned int)>& progress) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "external import requires an authenticated card session");
  }
  SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  // 🔴 Only a volume this session opened for maintenance. An ordinary mount
  // takes its files through the file manager, which the page already drives;
  // widening this entry would give a shell drag a way into any mounted volume
  // without the page having asked for one.
  if (!entry->mounted || !entry->maintenance) {
    throw std::logic_error(
        "external import requires a volume open for maintenance");
  }
  // The privileged import protocol authenticates regular backing files only.
  // A partition must never be substituted as a file path or weaken that gate.
  if (entry->partition)
    throw std::logic_error("partition maintenance external import is not supported");
  // The only thing taken from the source path is the name it will have inside
  // the volume, and that name is then checked as any other page-supplied
  // component is. A root, a trailing separator or a `..` leaves nothing that
  // can be named, so the import is refused instead of guessing.
  const std::wstring name =
      std::filesystem::path(absolute_source).filename().wstring();
  if (name.empty()) {
    throw std::invalid_argument("the dropped entry has no file name");
  }
  const auto destination = volume::resolve_volume_child_path(entry->drive_letter, relative_directory, name).substr(3);
  if (!entry->maintenance_identity || state_.decoy_outer_password.empty() || entry->maintenance_recovery_required)
    throw std::logic_error("maintenance upload requires a fresh authenticated mount");
  try {
    const auto verify = [&] {
      if (!authentication_.authenticated() || runtime_.mount_identity(entry->drive_letter) != *entry->maintenance_identity)
        throw std::runtime_error("maintenance identity changed during upload");
    };
    verify();
    SESSION_TRACE("开始导入到维护卷", container_text(*entry) + "；来源 " + wide_text(absolute_source) +
                                          "；目标 " + wide_text(destination));
    runtime_.import_protected(entry->path, *entry->maintenance_identity, state_.decoy_outer_password,
        hidden_container_password(), absolute_source, destination, cancelled, progress);
    verify();
    SESSION_TRACE("导入到维护卷完成", "来源 " + wide_text(absolute_source));
  } catch (...) {
    SESSION_TRACE_FAILURE("导入到维护卷失败", container_text(*entry) + "；来源 " +
                                                wide_text(absolute_source));
    entry->maintenance_recovery_required = true;
    runtime_.end_imports();
    throw;
  }
}

wchar_t CardLoginMountSessionCore::open_surveyed_maintenance(
    const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "decoy maintenance requires an authenticated card session");
  }
  if (state_.decoy_outer_password.empty()) {
    throw std::logic_error("this session has no surveyed decoy password");
  }
  SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (entry->mounted && entry->maintenance) {
    if (entry->maintenance_recovery_required) throw std::logic_error("end failed maintenance before reopening");
    return entry->drive_letter;
  }
  const bool switched = release_other_maintenance(id);
  try {
    return mount_outer_for_maintenance(*entry, state_.decoy_outer_password,
                                       MaintenanceMount::open);
  } catch (...) {
    SESSION_TRACE_FAILURE("打开已搜索的维护卷失败", container_text(*entry));
    if (switched) runtime_.end_measurements();
    throw;
  }
}

bool CardLoginMountSessionCore::release_other_maintenance(
    const std::uint32_t next_id) {
  bool released = false;
  for (const SessionContainer& container : state_.containers) {
    if (container.id == next_id || !container.maintenance) continue;
    SESSION_TRACE("切换维护卷，先卸下原维护卷", container_text(container));
    unmount_container_impl(container.id, false, false);
    released = true;
  }
  return released;
}

bool CardLoginMountSessionCore::verify_decoy_maintenance(
    const std::uint32_t id,
    const security::SecureBuffer& decoy_password_utf8) {
  SessionContainer& entry = maintenance_target(id, decoy_password_utf8);
  security::SecureBuffer outer_password = runtime_.derive_container_password(
      decoy_password_utf8, authentication_.root_secret_,
      layout_.container_b.salt);
  wchar_t drive_letter{};
  try {
    drive_letter = mount_outer_for_maintenance(entry, outer_password,
                                               MaintenanceMount::trial);
  } catch (const DecoyMaintenanceRefused&) {
    USB_VAULT_TRACE_FAILURE("application/card_login_mount_session", "异常已处理", "行 " + std::to_string(__LINE__) + "；函数 " + std::string(__func__));
    SESSION_TRACE("验证诱饵口令：不匹配", container_text(entry));
    outer_password.clear();
    return false;
  } catch (...) {
    SESSION_TRACE_FAILURE("验证诱饵口令失败", container_text(entry));
    outer_password.clear();
    throw;
  }
  outer_password.clear();
  SESSION_TRACE("验证诱饵口令：匹配", container_text(entry));

  // Nothing stays up on the strength of having asked. A volume that will not
  // come down stays recorded as mounted and the failure reaches the caller.
  static_cast<void>(runtime_.unmount_forced(drive_letter));
  mark_released(entry);
  return true;
}

void CardLoginMountSessionCore::complete_initialization(security::SecureBuffer password_utf8) {
  if (!authentication_.authenticated() || password_utf8.empty() ||
      state_.mounted() || !state_.containers.empty() || !state_.login_password.empty()) {
    throw std::logic_error("initialization handoff requires a fresh authenticated session");
  }
  // Move the exact normalized password confirmed before card initialization.
  // No second card authentication, key derivation, or automatic mount occurs.
  state_.login_password = std::move(password_utf8);
  SESSION_TRACE("初始化交接完成", "登录口令长度 " + std::to_string(state_.login_password.size()) + " 字节");
  clear_operation_history();
  record_operation(OperationKind::session_opened, OperationOutcome::succeeded);
}

security::SecureBuffer CardLoginMountSessionCore::copy_session_password() {
  if (!authentication_.authenticated()) return security::SecureBuffer();
  return duplicate_secret(state_.login_password);
}

void CardLoginMountSessionCore::release_session_secrets() noexcept {
  SESSION_TRACE("释放会话秘密", "登录口令 " + yes_no(!state_.login_password.empty()) +
                                    "；登录派生 " + yes_no(!state_.hidden_container_password.empty()) +
                                    "；诱饵派生 " + yes_no(!state_.decoy_outer_password.empty()));
  for (auto& container : state_.containers) container.maintenance_identity.reset();
  state_.hidden_container_password = security::SecureBuffer();
  state_.decoy_outer_password = security::SecureBuffer();
  state_.login_password = security::SecureBuffer();
  // The elevated runs belonged to the session that opened them.
  runtime_.end_imports();
  runtime_.end_measurements();
}

void CardLoginMountSessionCore::mount_adopted_container(
    const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "mounting a container requires an authenticated card session");
  }
  SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  SESSION_TRACE("挂载已收录的容器", container_text(*entry));
  if (entry->mounted) {
    SESSION_TRACE("容器已挂载，无需再挂", container_text(*entry));
    return;
  }
  if (entry->conversion_state) throw std::logic_error("incomplete conversion cannot mount");
  if (entry->partition) {
    const wchar_t drive = reserve_drive_letter();
    // Written out like the file mounts, so a change to the defaults cannot
    // reach partitions alone.
    const volume::MountOptions options{.drive_letter = drive,
                                       .prf = volume::Pkcs5Prf::sha512,
                                       .pim = 0,
                                       .read_only = false,
                                       .notify_shell = false};
    bool login_derived = entry->derivation_known && entry->release_first;
    volume::MountResult mounted{};
    if (!login_derived) {
      auto password = derive_standard_password();
      if (password.size() != security::kKdfV1OutputSize) throw std::logic_error("invalid partition password");
      SESSION_TRACE("以标准派生挂载分区", options_text(options));
      try { mounted = runtime_.mount_partition(*entry->partition, password, options); }
      catch (const std::system_error& error) {
        SESSION_TRACE_FAILURE("标准派生挂载分区失败", container_text(*entry));
        if (entry->derivation_known || state_.login_password.empty() ||
            error.code() != volume::make_error_code(volume::DriverError::authentication_failed)) throw;
        login_derived = true;
        SESSION_TRACE("标准派生被拒，改用登录派生挂载分区", "");
      } catch (...) {
        SESSION_TRACE_FAILURE("标准派生挂载分区失败", container_text(*entry));
        throw;
      }
    }
    if (login_derived) {
      const auto& password = hidden_container_password();
      if (password.size() != security::kKdfV1OutputSize) throw std::logic_error("invalid partition password");
      SESSION_TRACE("以登录派生挂载分区", options_text(options));
      try {
        mounted = runtime_.mount_partition(*entry->partition, password, options);
      } catch (...) {
        SESSION_TRACE_FAILURE("登录派生挂载分区失败", container_text(*entry));
        throw;
      }
    }
    SESSION_TRACE("分区挂载返回", mount_result_text(mounted));
    const wchar_t settled = settled_drive_letter(mounted);
    entry->drive_letter = settled;
    entry->mounted = true;
    entry->release_first = login_derived;
    entry->derivation_known = true;
    record_operation(OperationKind::container_mounted, OperationOutcome::succeeded, entry->name, settled);
    runtime_.notify_mounted(settled);
    SESSION_TRACE("分区已挂载", container_text(*entry));
    return;
  }
  const std::wstring container_name = entry->name;
  security::SecureBuffer standard_password;
  bool mounted = false;
  try {
    if (!runtime_.container_exists(entry->path)) {
      SESSION_TRACE("容器文件已不存在", "路径 " + wide_text(entry->path));
      throw std::system_error(
          std::make_error_code(std::errc::no_such_file_or_directory),
          "adopted container file is no longer present");
    }

    // A container read back from the index has never said which of its
    // volumes answers, so it is opened the way an import is: standard
    // derivation first, login derivation second. What answers is recorded, and
    // the next mount in this session no longer probes.
    wchar_t drive_letter{};
    bool hidden_volume = entry->release_first;
    if (!entry->derivation_known) {
      const ProbedMount probed = mount_with_either_derivation(entry->path);
      drive_letter = probed.drive_letter;
      hidden_volume = probed.hidden_volume;
    } else {
      if (hidden_volume && state_.login_password.empty()) {
        throw std::logic_error(
            "this container can only be opened by the login that captured it");
      }
      if (!hidden_volume) standard_password = derive_standard_password();
      const security::SecureBuffer& volume_password =
          hidden_volume ? hidden_container_password() : standard_password;
      const volume::MountOptions options{.drive_letter = reserve_drive_letter(),
                                         .prf = volume::Pkcs5Prf::sha512,
                                         .pim = 0,
                                         .read_only = false,
                                         .notify_shell = false};
      SESSION_TRACE(hidden_volume ? "以登录派生挂载文件容器" : "以标准派生挂载文件容器",
                    options_text(options));
      const volume::MountResult mount_result =
          runtime_.mount_created(entry->path, volume_password, options);
      SESSION_TRACE("文件容器挂载返回", mount_result_text(mount_result));
      drive_letter = settled_drive_letter(mount_result);
    }
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     drive_letter);
    standard_password.clear();
    entry->drive_letter = drive_letter;
    entry->mounted = true;
    entry->release_first = hidden_volume;
    entry->derivation_known = true;
    runtime_.notify_mounted(drive_letter);
    SESSION_TRACE("文件容器已挂载", container_text(*entry));
  } catch (...) {
    SESSION_TRACE_FAILURE("挂载文件容器失败", container_text(*entry));
    standard_password.clear();
    if (!mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::mount_container(
    const std::wstring_view path) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "mounting a container requires an authenticated card session");
  }
  const std::wstring container_name = file_name_of(path);
  if (const SessionContainer* const adopted = find_by_path(state_, path);
      adopted != nullptr && adopted->mounted) {
    throw std::logic_error("container is already mounted");
  }
  bool mounted = false;
  try {
    if (!runtime_.container_exists(path)) {
      throw std::system_error(
          std::make_error_code(std::errc::no_such_file_or_directory),
          "container file is unavailable");
    }
    SESSION_TRACE("按路径挂载容器", "路径 " + wide_text(path));
    const ProbedMount probed = mount_with_either_derivation(path);
    mounted = true;
    record_operation(OperationKind::container_mounted,
                     OperationOutcome::succeeded,
                     container_name,
                     probed.drive_letter);
    const std::uint32_t id = adopt(state_, path, probed.drive_letter, true,
                                   probed.hidden_volume, true, true)
                                 .id;
    remember_container_path(path);
    runtime_.notify_mounted(probed.drive_letter);
    if (const auto* entry = find_by_id(state_, id)) SESSION_TRACE("按路径挂载完成", container_text(*entry));
    return id;
  } catch (...) {
    SESSION_TRACE_FAILURE("按路径挂载失败", "路径 " + wide_text(path));
    if (!mounted) {
      record_operation(OperationKind::container_mounted,
                       OperationOutcome::failed,
                       container_name);
    }
    throw;
  }
}

std::uint32_t CardLoginMountSessionCore::adopt_container(
    const std::wstring_view path) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "adopting a container requires an authenticated card session");
  }
  if (const SessionContainer* const adopted = find_by_path(state_, path);
      adopted != nullptr) {
    return adopted->id;
  }
  if (!runtime_.container_exists(path)) {
    throw std::system_error(
        std::make_error_code(std::errc::no_such_file_or_directory),
        "container file is unavailable");
  }
  // Nothing is recorded in the session log: no volume was touched, and the
  // row appearing in the list is the whole of what happened. This is the same
  // arrival a remembered path gets at login, and it says the same amount
  // about the container -- which is nothing, until something mounts it.
  const std::uint32_t id =
      adopt(state_, path, wchar_t{}, false, false, false, true).id;
  SESSION_TRACE("导入容器", "容器 #" + std::to_string(id) + "；路径 " + wide_text(path));
  remember_container_path(path);
  return id;
}

bool CardLoginMountSessionCore::mounting_imported_containers() {
  return runtime_.mount_imported_containers();
}

void CardLoginMountSessionCore::set_mounting_imported_containers(
    const bool mount) {
  runtime_.set_mount_imported_containers(mount);
  SESSION_TRACE("导入后自动挂载设置已更改", "开启 " + yes_no(mount));
}

void CardLoginMountSessionCore::unmount_container(const std::uint32_t id) {
  unmount_container_impl(id, false);
}

void CardLoginMountSessionCore::force_unmount_container(const std::uint32_t id) {
  unmount_container_impl(id, true);
}

void CardLoginMountSessionCore::force_unmount_user_active() {
  SESSION_TRACE("强制卸载全部容器", "");
  for (const bool release_first : {true, false}) {
    for (const SessionContainer& container : state_.containers) {
      if (container.mounted && container.release_first == release_first)
        unmount_container_impl(container.id, true);
    }
  }
}

void CardLoginMountSessionCore::unmount_container_impl(
    const std::uint32_t id, const bool allow_force,
    const bool end_measurements) {
  for (SessionContainer& container : state_.containers) {
    if (container.id != id) continue;
    if (!container.mounted) return;
    const wchar_t drive_letter = container.drive_letter;
    const bool ended_maintenance = container.maintenance;
    SESSION_TRACE(allow_force ? "强制卸载容器" : "卸载容器", container_text(container));
    try {
      if (allow_force) {
        const volume::UnmountEscalationResult result = runtime_.unmount_forced(drive_letter);
        SESSION_TRACE("卸载返回", "已升级为强制 " + yes_no(result.forced) + "；" +
                                      unmount_result_text(result.result));
      } else {
        const volume::UnmountResult result = runtime_.unmount(drive_letter);
        SESSION_TRACE("卸载返回", unmount_result_text(result));
      }
      mark_released(container);
      if (ended_maintenance) runtime_.end_imports();
      if (ended_maintenance && end_measurements) runtime_.end_measurements();
      record_operation(OperationKind::container_unmounted,
                       OperationOutcome::succeeded,
                       container.name,
                       drive_letter);
      SESSION_TRACE("容器已卸载", container_text(container) + "；原盘符 " + drive_text(drive_letter) +
                                    "；结束维护任务 " + yes_no(ended_maintenance));
      return;
    } catch (...) {
      SESSION_TRACE_FAILURE(allow_force ? "强制卸载容器失败" : "卸载容器失败", container_text(container));
      record_operation(OperationKind::container_unmounted,
                       OperationOutcome::failed,
                       container.name,
                       drive_letter);
      throw;
    }
  }
  throw std::invalid_argument("no container is adopted under that identifier");
}

void CardLoginMountSessionCore::forget_container(const std::uint32_t id) {
  for (auto entry = state_.containers.begin();
       entry != state_.containers.end(); ++entry) {
    if (entry->id != id) continue;
    if (entry->conversion_state) throw std::logic_error("incomplete conversion record cannot be forgotten");
    if (entry->mounted) {
      throw std::logic_error(
          "a mounted container cannot be dropped from the session");
    }
    // The path is copied out before the entry goes, because forgetting it is
    // what the caller asked for and the entry is where it is written down.
    SESSION_TRACE("从列表移除容器", container_text(*entry));
    if (entry->partition) {
      auto key = container_index_key();
      auto index = runtime_.read_container_index();
      const bool removed = remove_container_index_partition(key.bytes(), index, *entry->partition);
      SESSION_TRACE("从容器索引移除分区", "找到 " + yes_no(removed));
      if (removed) runtime_.write_container_index(index);
      state_.containers.erase(entry);
      return;
    }
    const std::wstring path = entry->path;
    state_.containers.erase(entry);
    forget_container_path(path);
    return;
  }
  throw std::invalid_argument("no container is adopted under that identifier");
}

wchar_t CardLoginMountSessionCore::mounted_drive_letter(
    const std::uint32_t id) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "reaching into a container requires an authenticated card session");
  }
  const SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (!entry->mounted) {
    throw std::logic_error(
        "that container is not mounted, so it has no contents to reach");
  }
  return entry->drive_letter;
}

volume::VolumeDirectoryListing
CardLoginMountSessionCore::list_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  const std::wstring directory =
      volume::resolve_volume_entry_path(drive_letter, relative_directory);
  SESSION_TRACE("列出目录", "容器 #" + std::to_string(id) + "；目录 " + wide_text(directory));
  return runtime_.list_directory(directory);
}

void CardLoginMountSessionCore::delete_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  const std::wstring target =
      volume::resolve_volume_child_path(drive_letter, relative_directory, name);
  SESSION_TRACE("删除条目", "容器 #" + std::to_string(id) + "；路径 " + wide_text(target));
  runtime_.delete_entry(target);
}

void CardLoginMountSessionCore::rename_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name, const std::wstring_view new_name) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  // Both ends are resolved before either is used, so a bad new name refuses
  // the rename rather than half-performing it.
  const std::wstring source = volume::resolve_volume_child_path(
      drive_letter, relative_directory, name);
  const std::wstring destination = volume::resolve_volume_child_path(
      drive_letter, relative_directory, new_name);
  SESSION_TRACE("重命名条目", "容器 #" + std::to_string(id) + "；来源 " + wide_text(source) +
                                  "；目标 " + wide_text(destination));
  runtime_.move_entry(source, destination);
}

void CardLoginMountSessionCore::create_container_directory(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  const std::wstring target =
      volume::resolve_volume_child_path(drive_letter, relative_directory, name);
  SESSION_TRACE("新建目录", "容器 #" + std::to_string(id) + "；路径 " + wide_text(target));
  runtime_.create_directory(target);
}

void CardLoginMountSessionCore::move_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  const wchar_t source_drive = mounted_drive_letter(source_id);
  const wchar_t destination_drive = mounted_drive_letter(destination_id);
  const std::wstring source = volume::resolve_volume_child_path(
      source_drive, source_relative_directory, name);
  const std::wstring destination = volume::resolve_volume_child_path(
      destination_drive, destination_relative_directory, name);
  // One identifier is one volume, and this session never puts two containers
  // on one letter, so the identifiers settle which move this is.
  SESSION_TRACE("移动条目", "来源容器 #" + std::to_string(source_id) + "；来源 " + wide_text(source) +
                                "；目标容器 #" + std::to_string(destination_id) + "；目标 " +
                                wide_text(destination) + "；跨卷复制 " + yes_no(source_id != destination_id));
  if (source_id == destination_id) {
    runtime_.move_entry(source, destination);
    return;
  }
  runtime_.move_entry_by_copying(source, destination);
}

void CardLoginMountSessionCore::copy_container_entry(
    const std::uint32_t source_id,
    const std::wstring_view source_relative_directory,
    const std::wstring_view name, const std::uint32_t destination_id,
    const std::wstring_view destination_relative_directory) {
  const wchar_t source_drive = mounted_drive_letter(source_id);
  const wchar_t destination_drive = mounted_drive_letter(destination_id);
  const std::wstring source = volume::resolve_volume_child_path(
      source_drive, source_relative_directory, name);
  const std::wstring destination = volume::resolve_volume_child_path(
      destination_drive, destination_relative_directory, name);
  SESSION_TRACE("复制条目", "来源容器 #" + std::to_string(source_id) + "；来源 " + wide_text(source) +
                                "；目标容器 #" + std::to_string(destination_id) + "；目标 " + wide_text(destination));
  runtime_.copy_entry(source, destination, {});
}

void CardLoginMountSessionCore::export_container_entry(
    const std::uint32_t id, const std::wstring_view relative_directory,
    const std::wstring_view name,
    const std::wstring_view absolute_destination_directory) {
  const wchar_t drive_letter = mounted_drive_letter(id);
  const std::wstring destination = volume::resolve_exported_entry_path(
      absolute_destination_directory, name);

  // 🔴 A location on a drive this session has a container mounted on is not an
  // export, it is a copy, and the file manager already does copies without any
  // of what this call implies. Refusing it here also keeps the one entry point
  // that may leave a volume from being the convenient way to do everything.
  for (const SessionContainer& container : state_.containers) {
    if (container.mounted &&
        upper_drive_letter(container.drive_letter) ==
            upper_drive_letter(destination[0])) {
      throw std::invalid_argument(
          "that location is inside a container, which is a copy and not an "
          "export");
    }
  }

  // 🔴 A location on a drive this session has a container mounted on is not an
  // export, it is a copy, and the file manager already does copies without any
  // of what this call implies. Refusing it here also keeps the one entry point
  // that may leave a volume from being the convenient way to do everything.
  // 🔴 No `record_operation` here, and none below it. The operation history is
  // a closed schema with no export in it, and Q-04 makes leaving it that way
  // this product's obligation.
  const std::wstring source =
      volume::resolve_volume_child_path(drive_letter, relative_directory, name);
  SESSION_TRACE("导出条目", "容器 #" + std::to_string(id) + "；来源 " + wide_text(source) +
                                "；目标 " + wide_text(destination));
  runtime_.copy_entry(source, destination, {});
}

void CardLoginMountSessionCore::export_container_file(
    const std::uint32_t id,
    const std::wstring_view absolute_destination_directory) {
  if (!authentication_.authenticated()) {
    throw std::logic_error(
        "migrating a container requires an authenticated card session");
  }
  const SessionContainer* const entry = find_by_id(state_, id);
  if (entry == nullptr) {
    throw std::invalid_argument(
        "no container is adopted under that identifier");
  }
  if (entry->partition) throw std::logic_error("partition ciphertext export is unsupported");
  // 🔴 A mounted container is one the driver is writing to, so a copy taken
  // now is a copy of something in flight. Q-04 forbids handing back an
  // incomplete copy as a usable migration result, and the only way to be sure
  // of that here is to refuse rather than to copy and hope.
  if (entry->mounted) {
    throw std::logic_error(
        "unmount the container before migrating it, so what is copied is a "
        "volume nothing is writing to");
  }

  // Byte for byte, under the container's own name. Nothing is decrypted,
  // nothing is re-encrypted, nothing is added: what comes out is the file that
  // went in. No `record_operation` here either -- a migration is an export,
  // and Q-04 says an export leaves no trace of itself behind.
  const std::wstring destination = volume::resolve_exported_entry_path(
      absolute_destination_directory, entry->name);
  SESSION_TRACE("导出容器文件", container_text(*entry) + "；目标 " + wide_text(destination));
  runtime_.copy_entry(entry->path, destination, {});
}

std::vector<ContainerStatus> CardLoginMountSessionCore::container_statuses() {
  std::vector<ContainerStatus> result;
  result.reserve(state_.containers.size());
  for (SessionContainer& container : state_.containers) {
    // The driver is the only authority on an attached volume: it alone knows
    // whether the hidden or the outer volume of a two-volume container is the
    // one currently on that drive. One row it cannot speak for never takes
    // the rest of the list down with it.
    std::optional<std::uint64_t> size;
    bool answered = container.mounted;
    if (container.mounted) {
      try {
        size = runtime_.mounted_volume_size(container.drive_letter);
      } catch (...) {
        SESSION_TRACE_FAILURE("查询已挂载卷失败", container_text(container));
        answered = false;
      }
    }
    if (answered && !size.has_value()) {
      // The driver answered and nothing is on that letter any more: the
      // volume was released outside this application. Say so rather than
      // keep a row that can neither be unmounted nor removed.
      SESSION_TRACE("盘符上已无卷，视为已在应用外卸载", container_text(container));
      const bool ended_maintenance = container.maintenance;
      mark_released(container);
      if (ended_maintenance) {
        runtime_.end_imports();
        runtime_.end_measurements();
      }
    }
    if (!container.mounted) {
      // An unmounted container has no capacity to report. The backing file
      // size is not a volume size, and for a container holding two volumes
      // which of them would appear is not decided until it is mounted.
      result.push_back({.id = container.id,
                        .name = container.name,
                        .mounted = false,
                        .size_bytes = 0,
                        .drive_letter = wchar_t{},
                        .partition = container.partition.has_value(), .conversion_state=container.conversion_state});
      continue;
    }
    // Either the query failed or it reported no capacity. The mount is not
    // disproved, so the row stays mounted and can still be unmounted.
    const bool unavailable = !size.has_value() || *size == 0;
    if (unavailable && answered) SESSION_TRACE("已挂载卷报告容量为 0", container_text(container));
    result.push_back({.id = container.id,
                      .name = container.name,
                      .mounted = true,
                      .size_bytes = unavailable ? 0 : *size,
                      .drive_letter = container.drive_letter,
                      .partition = container.partition.has_value(), .conversion_state=container.conversion_state,
                      .volume_unavailable = unavailable});
  }
  return result;
}

std::vector<ContainerOperationTarget> CardLoginMountSessionCore::container_operation_targets() {
  std::vector<ContainerOperationTarget> result;
  if (!authentication_.authenticated()) return result;
  for (const auto& entry : state_.containers)
    result.push_back({entry.id, entry.name, entry.mounted});
  return result;
}

ContainerAStatus CardLoginMountSessionCore::container_a_status() {
  const std::optional<std::uint64_t> size =
      runtime_.container_size_if_present(layout_.container_a.path);
  const SessionContainer* const adopted =
      find_by_path(state_, layout_.container_a.path);
  const bool mounted = adopted != nullptr && adopted->mounted;
  if (!size.has_value()) {
    if (mounted) {
      throw std::runtime_error(
          "mounted container A backing file is unavailable");
    }
    return {};
  }
  return {.exists = true,
          .mounted = mounted,
          .size_bytes = *size,
          .drive_letter = mounted ? adopted->drive_letter : wchar_t{}};
}

void CardLoginMountSessionCore::unmount_active() {
  // The B-before-A order required after authentication still holds: the
  // container with a hidden volume is released first, then every other one.
  for (const bool release_first : {true, false}) {
    for (SessionContainer& container : state_.containers) {
      if (!container.mounted || container.release_first != release_first) {
        continue;
      }
      const wchar_t drive_letter = container.drive_letter;
      const bool ended_maintenance = container.maintenance;
      SESSION_TRACE("全部卸载：卸载容器", container_text(container));
      try {
        const volume::UnmountResult result = runtime_.unmount(drive_letter);
        SESSION_TRACE("全部卸载：卸载返回", unmount_result_text(result));
        mark_released(container);
        if (ended_maintenance) { runtime_.end_imports(); runtime_.end_measurements(); }
        record_operation(OperationKind::container_unmounted,
                         OperationOutcome::succeeded,
                         container.name,
                         drive_letter);
      } catch (...) {
        SESSION_TRACE_FAILURE("全部卸载：卸载容器失败", container_text(container));
        record_operation(OperationKind::container_unmounted,
                         OperationOutcome::failed,
                         container.name,
                         drive_letter);
        throw;
      }
    }
  }
}

bool CardLoginMountSessionCore::force_unmount_active() {
  // Forced release is the card-removal path, so every retained secret goes
  // before any volume is asked to leave, and a refusal does not bring a
  // secret back. One volume that will not leave must not leave the others
  // mounted. A deliberate unmount keeps the secrets, which is what lets the
  // user mount a container again in the same session.
  SESSION_TRACE("拔卡清理开始", "已挂载容器 " + yes_no(state_.mounted()));
  release_session_secrets();
  clear_operation_history();
  bool forced = false;
  std::exception_ptr failure;
  for (const bool release_first : {true, false}) {
    for (SessionContainer& container : state_.containers) {
      if (!container.mounted || container.release_first != release_first) {
        continue;
      }
      volume::UnmountEscalationResult result{};
      SESSION_TRACE("拔卡清理：卸载容器", container_text(container));
      try {
        result = runtime_.unmount_forced(container.drive_letter);
      } catch (...) {
        SESSION_TRACE_FAILURE("拔卡清理：卸载容器失败", container_text(container));
        if (!failure) failure = std::current_exception();
        continue;
      }
      SESSION_TRACE("拔卡清理：卸载返回", "已升级为强制 " + yes_no(result.forced) + "；" +
                                            unmount_result_text(result.result));
      forced = result.forced || forced;
      mark_released(container);
    }
  }
  SESSION_TRACE("拔卡清理结束", "有卷被强制卸下 " + yes_no(forced) + "；仍有卷挂着 " +
                                    yes_no(state_.mounted()) + "；有失败 " + yes_no(static_cast<bool>(failure)));
  if (failure) std::rethrow_exception(failure);
  return forced;
}

bool CardLoginMountSessionCore::mounted() const noexcept {
  return state_.mounted();
}

}  // namespace detail
}  // namespace usb_vault::application
