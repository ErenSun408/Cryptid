#pragma once
#include "volume/partition_inplace_runner.h"
#include "volume_creation/veracrypt_format_abi.h"
#include <functional>
namespace usb_vault::volume_creation::detail {
// Caller retains DLL, device handle and password throughout this synchronous run.
// Must authorize/confirm/revalidate before constructing. No path reopen here.
class PartitionInplaceBackend final : public volume::detail::InplaceBackend {
 public:
  PartitionInplaceBackend(HMODULE module, HANDLE target, std::uint64_t offset,
      std::uint64_t size, const security::SecureBuffer& password, bool begin,
      std::function<volume::detail::InplaceControl()> control, ProgressCallback progress = {});
  ~PartitionInplaceBackend() override { close(); }
  volume::detail::InplaceControl control() override;
  volume::detail::InplaceRange open_authenticated() override;
  std::size_t read(std::uint64_t offset, std::span<std::byte> buffer) override;
  void encrypt(std::uint64_t unit, std::span<std::byte> buffer) override;
  std::size_t write(std::uint64_t offset, std::span<const std::byte> buffer) override;
  void save_progress(const volume::detail::InplaceRange& range) override;
  void flush() override;
  void finalize() override;
  void close() noexcept override;
 private:
  HANDLE target_; std::uint64_t offset_, size_, converted_{};
  const security::SecureBuffer& password_; bool begin_, opened_{};
  std::function<volume::detail::InplaceControl()> control_; ProgressCallback progress_;
  format_abi::InplaceOpen open_; format_abi::InplaceEncrypt encrypt_;
  format_abi::InplaceSave save_; format_abi::InplaceFinalize finalize_; format_abi::InplaceClose close_;
};
}
