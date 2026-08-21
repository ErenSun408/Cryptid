#pragma once

#include <cstddef>
#include <type_traits>

#define USB_VAULT_ASSERT_STANDARD_LAYOUT(type)                                \
  static_assert(std::is_standard_layout_v<type>,                              \
                #type " must use a stable standard-layout ABI")

#define USB_VAULT_ASSERT_SIZE(type, expected_size)                            \
  static_assert(sizeof(type) == (expected_size),                              \
                #type " has an unexpected ABI size")

#define USB_VAULT_ASSERT_OFFSET(type, member, expected_offset)                \
  static_assert(offsetof(type, member) == (expected_offset),                  \
                #type "::" #member " has an unexpected ABI offset")
