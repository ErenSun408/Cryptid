#include "usb_vault/security/secure_buffer.h"

#include <Windows.h>

#include <system_error>
#include <utility>

namespace usb_vault::security {
namespace {

[[noreturn]] void throw_windows_error(const DWORD error,
                                      const char* const operation) {
  throw std::system_error(static_cast<int>(error), std::system_category(),
                          operation);
}

}  // namespace

SecureBuffer::SecureBuffer(const std::size_t size) {
  if (size == 0) {
    return;
  }

  void* const allocation =
      ::VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (allocation == nullptr) {
    throw_windows_error(::GetLastError(), "VirtualAlloc");
  }

  if (::VirtualLock(allocation, size) == FALSE) {
    const DWORD error = ::GetLastError();
    static_cast<void>(::VirtualFree(allocation, 0, MEM_RELEASE));
    throw_windows_error(error, "VirtualLock");
  }

  data_ = static_cast<value_type*>(allocation);
  size_ = size;
}

SecureBuffer::~SecureBuffer() noexcept { release(); }

SecureBuffer::SecureBuffer(SecureBuffer&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

SecureBuffer& SecureBuffer::operator=(SecureBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  release();
  data_ = std::exchange(other.data_, nullptr);
  size_ = std::exchange(other.size_, 0);
  return *this;
}

SecureBuffer::value_type* SecureBuffer::data() noexcept { return data_; }

const SecureBuffer::value_type* SecureBuffer::data() const noexcept {
  return data_;
}

std::size_t SecureBuffer::size() const noexcept { return size_; }

bool SecureBuffer::empty() const noexcept { return size_ == 0; }

std::span<SecureBuffer::value_type> SecureBuffer::bytes() noexcept {
  return {data_, size_};
}

std::span<const SecureBuffer::value_type> SecureBuffer::bytes() const noexcept {
  return {data_, size_};
}

void SecureBuffer::clear() noexcept {
  if (data_ != nullptr) {
    ::SecureZeroMemory(data_, size_);
  }
}

void SecureBuffer::release() noexcept {
  if (data_ == nullptr) {
    return;
  }

  clear();
  static_cast<void>(::VirtualUnlock(data_, size_));
  static_cast<void>(::VirtualFree(data_, 0, MEM_RELEASE));
  data_ = nullptr;
  size_ = 0;
}

}  // namespace usb_vault::security
