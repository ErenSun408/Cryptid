#pragma once

#include "usb_vault/security/secure_buffer.h"

#include <memory>
#include <type_traits>

namespace usb_vault::volume::detail {

template <typename T>
class LockedDriverBuffer final {
 public:
  static_assert(std::is_trivially_destructible_v<T>);

  LockedDriverBuffer()
      : storage_(sizeof(T)),
        value_(std::construct_at(reinterpret_cast<T*>(storage_.data()))) {}

  ~LockedDriverBuffer() noexcept { std::destroy_at(value_); }

  LockedDriverBuffer(const LockedDriverBuffer&) = delete;
  LockedDriverBuffer& operator=(const LockedDriverBuffer&) = delete;
  LockedDriverBuffer(LockedDriverBuffer&&) = delete;
  LockedDriverBuffer& operator=(LockedDriverBuffer&&) = delete;

  [[nodiscard]] T& get() noexcept { return *value_; }
  [[nodiscard]] const T& get() const noexcept { return *value_; }
  void clear() noexcept { storage_.clear(); }

 private:
  security::SecureBuffer storage_;
  T* value_;
};

}  // namespace usb_vault::volume::detail
