cmake_minimum_required(VERSION 3.25)

project(usb_vault LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "usb-vault currently supports Windows only")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "usb-vault requires a 64-bit toolchain")
endif()

option(USB_VAULT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(USB_VAULT_ENABLE_CODE_ANALYSIS "Enable MSVC code analysis" OFF)
option(USB_VAULT_ENABLE_INTEGRATION_TESTS
       "Enable tests that mount a local VeraCrypt fixture" OFF)

function(usb_vault_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4
      /WX
      /permissive-
      /Zc:__cplusplus
      /utf-8
      /sdl
    )

    if(USB_VAULT_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address /INCREMENTAL:NO)

      get_target_property(target_type ${target} TYPE)
      if(target_type STREQUAL "EXECUTABLE")
        get_filename_component(msvc_tool_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
          set(asan_runtime_name clang_rt.asan_dbg_dynamic-x86_64.dll)
        else()
          set(asan_runtime_name clang_rt.asan_dynamic-x86_64.dll)
        endif()
        set(asan_runtime "${msvc_tool_directory}/${asan_runtime_name}")
        if(NOT EXISTS "${asan_runtime}")
          message(FATAL_ERROR "MSVC AddressSanitizer runtime not found: ${asan_runtime}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "${asan_runtime}"
                  "$<TARGET_FILE_DIR:${target}>"
          VERBATIM
        )
      endif()
    endif()

    if(USB_VAULT_ENABLE_CODE_ANALYSIS)
      target_compile_options(${target} PRIVATE /analyze)
    endif()
  else()
    message(FATAL_ERROR "P1.0 requires the MSVC toolchain")
  endif()
endfunction()

add_library(argon2_ref STATIC
  third_party/argon2/src/argon2.c
  third_party/argon2/src/blake2/blake2b.c
  third_party/argon2/src/core.c
  third_party/argon2/src/encoding.c
  third_party/argon2/src/ref.c
  third_party/argon2/src/thread.c
)

target_include_directories(argon2_ref
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/argon2/src
)

target_compile_definitions(argon2_ref PRIVATE ARGON2_NO_THREADS)
set_target_properties(argon2_ref PROPERTIES
  C_STANDARD 90
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF
)

if(MSVC)
  target_compile_options(argon2_ref PRIVATE /utf-8)
  if(USB_VAULT_ENABLE_ASAN)
    target_compile_options(argon2_ref PRIVATE /fsanitize=address)
  endif()
endif()

add_library(usb_vault_core STATIC
  src/application/mount_orchestrator.cpp
  src/security/kdf_v1.cpp
  src/security/secure_buffer.cpp
  src/volume/mount_request.cpp
  src/volume/unmount_retry.cpp
  src/volume/volume_provider.cpp
)

target_include_directories(usb_vault_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_core PRIVATE argon2_ref Shell32)

usb_vault_configure_target(usb_vault_core)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
