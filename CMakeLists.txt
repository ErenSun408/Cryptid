cmake_minimum_required(VERSION 3.25)

project(usb_vault VERSION 0.2.0 LANGUAGES C CXX)

# CMake learns Ninja's MSVC /showIncludes marker while identifying the
# compiler. On a Chinese Windows console configured with a legacy code page,
# some CMake versions decode that marker as UTF-8 mojibake. Ninja then cannot
# recognize any header dependency: header-only edits appear to need no work,
# and stale objects can survive ABI changes. Fail during configuration instead
# of allowing a build tree with silently invalid dependency tracking.
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  execute_process(
    COMMAND cmd.exe /d /c chcp
    RESULT_VARIABLE code_page_result
    OUTPUT_VARIABLE code_page_output
    ERROR_VARIABLE code_page_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  string(REGEX MATCH "[0-9]+" active_code_page "${code_page_output}")
  if(NOT code_page_result EQUAL 0 OR NOT active_code_page STREQUAL "65001")
    message(FATAL_ERROR
      "MSVC/Ninja configuration requires Windows code page 65001 so CMake "
      "and Ninja agree on the /showIncludes encoding (detected output: "
      "'${code_page_output}${code_page_error}'). Use "
      "tools/configure-preset.cmd <preset> for repository presets.")
  endif()

  foreach(language IN ITEMS C CXX)
    set(prefix_variable "CMAKE_${language}_CL_SHOWINCLUDES_PREFIX")
    if(NOT DEFINED ${prefix_variable} OR "${${prefix_variable}}" STREQUAL "")
      message(FATAL_ERROR
        "MSVC/Ninja did not report a /showIncludes dependency prefix for "
        "${language}; refusing to generate a build with unverified header "
        "dependency tracking")
    endif()

  endforeach()
endif()

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
option(USB_VAULT_BUILD_UI "Build the minimal WebView2 application" ON)
set(USB_VAULT_FORMAT_DLL "" CACHE FILEPATH
    "Optional patched VeraCryptFormat.dll copied beside the helper")

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
  src/application/card_authentication_session.cpp
  src/application/card_login_mount_session.cpp
  src/application/card_device_lifecycle_controller.cpp
  src/application/card_entry_context_controller.cpp
  src/application/card_entry_router.cpp
  src/application/card_presence_controller.cpp
  src/application/container_index_store_v1.cpp
  src/application/container_index_v1.cpp
  src/application/container_index_view_v1.cpp
  src/application/mount_orchestrator.cpp
  src/application/product_settings_v1.cpp
  src/card/card_error.cpp
  src/card/card_initialization_service.cpp
  src/card/card_record_journal_v1.cpp
  src/card/card_record_service.cpp
  src/card/card_record_v1.cpp
  src/card/card_record_v1_inspection.cpp
  src/security/argon2id_runtime.cpp
  src/security/confirmed_password.cpp
  src/security/kdf_v1.cpp
  src/security/offline_card_password_policy.cpp
  src/security/password_normalization.cpp
  src/security/secure_buffer.cpp
  src/ui/card_product_webview_host_object.cpp
  src/ui/card_product_ui_session_controller.cpp
  src/ui/container_target_path.cpp
  src/ui/password_ingress.cpp
  src/ui/ui_close_guard.cpp
  src/ui/webview_password_array.cpp
  src/volume/mount_request.cpp
  src/volume/unmount_retry.cpp
  src/volume/volume_containment.cpp
  src/volume/volume_directory_listing.cpp
  src/volume/volume_entry_path.cpp
  src/volume/volume_file_operations.cpp
  src/volume/volume_provider.cpp
  src/volume_creation/creator_protocol.cpp
  src/volume_creation/creator_pipe_security.cpp
  src/volume_creation/volume_creator.cpp
)

target_include_directories(usb_vault_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_core PRIVATE
  argon2_ref
  Advapi32
  Bcrypt
  Ole32
  OleAut32
  Shell32
)

usb_vault_configure_target(usb_vault_core)

# Windows HID notification delivery is kept outside the vendor-neutral core.
# Consumers provide a VID/PID identity; the adapter never links a vendor SDK
# or performs HID report I/O.
add_library(usb_vault_windows_hid_notifications STATIC
  src/platform/windows/hid_card_device_notifications.cpp
)

target_include_directories(usb_vault_windows_hid_notifications
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_windows_hid_notifications
  PUBLIC
    usb_vault_core
  PRIVATE
    Hid
    Setupapi
    User32
)

usb_vault_configure_target(usb_vault_windows_hid_notifications)

# Shell file dialogs stay outside the core for the same reason as the HID
# adapter: the core must not depend on Windows UI. The adapter only returns a
# path the user picked and never records where that path was.
add_library(usb_vault_windows_shell_container_chooser STATIC
  src/platform/windows/shell_container_file_chooser.cpp
)

target_include_directories(usb_vault_windows_shell_container_chooser
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_windows_shell_container_chooser
  PUBLIC
    usb_vault_core
  PRIVATE
    Ole32
    Shell32
)

usb_vault_configure_target(usb_vault_windows_shell_container_chooser)

# Local artifact identity and replacement locks are shared by the vendor DLL
# verifier and the isolated helper launcher without linking either consumer to
# the other.
add_library(usb_vault_verified_local_artifact STATIC
  src/card/verified_local_artifact.cpp
)

target_include_directories(usb_vault_verified_local_artifact
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_verified_local_artifact PRIVATE
  usb_vault_core
  Bcrypt
)
usb_vault_configure_target(usb_vault_verified_local_artifact)

# The formal ViKey ICardStore adapters are kept independent from both the
# vendor SDK and helper transport. Separate helper-backed implementations below
# supply the private legacy-blob and P2 card-record backends without changing
# the application-facing single-blob contract.
add_library(usb_vault_vikey_card_store STATIC
  src/card/vikey/vikey_card_record_store.cpp
  src/card/vikey/vikey_card_store.cpp
)

target_include_directories(usb_vault_vikey_card_store
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_vikey_card_store PUBLIC usb_vault_core)
usb_vault_configure_target(usb_vault_vikey_card_store)

# The product executable intentionally does not link this target. SDK-bound
# session code remains private to the ViKey adapter/helper boundary; unit tests
# use injected function tables and never load ViKey.dll.
add_library(usb_vault_vikey_sdk STATIC
  src/card/vikey/vikey_artifact.cpp
  src/card/vikey/vikey_blob_round_trip.cpp
  src/card/vikey/vikey_card_record_io.cpp
  src/card/vikey/vikey_module.cpp
  src/card/vikey/vikey_session.cpp
)

target_include_directories(usb_vault_vikey_sdk
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_vikey_sdk PRIVATE
  usb_vault_core
  usb_vault_verified_local_artifact
)

usb_vault_configure_target(usb_vault_vikey_sdk)

# Versioned IPC is shared by the launcher and the helper, while the launcher
# remains separate from both the product executable and the vendor SDK target.
add_library(usb_vault_vikey_helper_protocol STATIC
  src/card/vikey/vikey_helper_protocol.cpp
)

target_include_directories(usb_vault_vikey_helper_protocol
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_vikey_helper_protocol PRIVATE usb_vault_core)
usb_vault_configure_target(usb_vault_vikey_helper_protocol)

add_library(usb_vault_vikey_helper_client STATIC
  src/card/vikey/vikey_helper_client.cpp
)

target_include_directories(usb_vault_vikey_helper_client
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_vikey_helper_client PRIVATE
  usb_vault_core
  usb_vault_vikey_helper_protocol
  usb_vault_verified_local_artifact
  Advapi32
  Bcrypt
  Ole32
  Shell32
  Userenv
)
usb_vault_configure_target(usb_vault_vikey_helper_client)

# The transport-backed implementation remains a separate development-only
# composition layer so the formal ICardStore adapter itself stays independent
# from helper IPC and the vendor runtime boundary.
add_library(usb_vault_vikey_helper_blob_backend STATIC
  src/card/vikey/vikey_helper_blob_backend.cpp
)

target_include_directories(usb_vault_vikey_helper_blob_backend
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_vikey_helper_blob_backend
  PUBLIC
    usb_vault_vikey_card_store
  PRIVATE
    usb_vault_vikey_helper_client
)
usb_vault_configure_target(usb_vault_vikey_helper_blob_backend)

add_library(usb_vault_vikey_helper_card_record_backend STATIC
  src/card/vikey/vikey_card_insertion_context.cpp
  src/card/vikey/vikey_helper_card_enrollment.cpp
  src/card/vikey/vikey_helper_card_record_backend.cpp
)

target_include_directories(usb_vault_vikey_helper_card_record_backend
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_vikey_helper_card_record_backend
  PUBLIC
    usb_vault_vikey_card_store
  PRIVATE
    usb_vault_vikey_helper_client
)
usb_vault_configure_target(
  usb_vault_vikey_helper_card_record_backend)

# Development-only Windows Credential Manager implementation of the formal
# helper backend's credential-provider boundary. Production provisioning is a
# separate deployment decision.
add_library(usb_vault_vikey_development_credentials STATIC EXCLUDE_FROM_ALL
  src/card/vikey/vikey_development_app_config.cpp
  src/card/vikey/vikey_development_credentials.cpp
)

target_include_directories(usb_vault_vikey_development_credentials
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(usb_vault_vikey_development_credentials
  PUBLIC
    usb_vault_core
  PRIVATE
    Advapi32
)
usb_vault_configure_target(usb_vault_vikey_development_credentials)

# Small current-user configurator for the three development-only Credential
# Manager entries. It is explicitly built and distributed only with the ViKey
# development package, never as part of the normal product target graph.
add_executable(usb_vault_vikey_development_credentials_config
  WIN32 EXCLUDE_FROM_ALL
  tools/vikey_development_credentials_config/main.cpp
)
target_include_directories(
  usb_vault_vikey_development_credentials_config PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(
  usb_vault_vikey_development_credentials_config PRIVATE UNICODE _UNICODE)
target_link_options(
  usb_vault_vikey_development_credentials_config PRIVATE
  "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
)
target_link_libraries(
  usb_vault_vikey_development_credentials_config PRIVATE
  usb_vault_vikey_development_credentials
)
usb_vault_configure_target(
  usb_vault_vikey_development_credentials_config)

add_executable(usb_vault_vikey_helper WIN32
  tools/vikey_helper/main.cpp
  src/card/verified_local_artifact.cpp
  src/card/vikey/vikey_helper_protocol.cpp
  src/card/vikey/vikey_hid_probe.cpp
  src/card/vikey/vikey_artifact.cpp
  src/card/vikey/vikey_blob_round_trip.cpp
  src/card/vikey/vikey_card_record_io.cpp
  src/card/vikey/vikey_module.cpp
  src/card/vikey/vikey_session.cpp
  src/card/card_record_journal_v1.cpp
  src/card/card_record_v1_inspection.cpp
  src/card/card_error.cpp
  src/security/secure_buffer.cpp
)

target_include_directories(usb_vault_vikey_helper PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(usb_vault_vikey_helper PRIVATE UNICODE _UNICODE)
set_property(TARGET usb_vault_vikey_helper PROPERTY MSVC_RUNTIME_LIBRARY
  "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_link_options(usb_vault_vikey_helper PRIVATE
  "/MANIFESTUAC:level='asInvoker' uiAccess='false'"
)
target_link_libraries(usb_vault_vikey_helper PRIVATE Bcrypt Hid Setupapi)
usb_vault_configure_target(usb_vault_vikey_helper)

# Explicitly built manual verifier for the development card. It reads all
# development credentials from Windows Credential Manager; it is never part of
# the product or default build.
add_executable(usb_vault_vikey_development_session_probe EXCLUDE_FROM_ALL
  tools/vikey_development_session_probe/main.cpp
)
target_include_directories(usb_vault_vikey_development_session_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(usb_vault_vikey_development_session_probe PRIVATE
  usb_vault_vikey_development_credentials
  usb_vault_vikey_helper_client
)
usb_vault_configure_target(usb_vault_vikey_development_session_probe)

# Explicitly built destructive verifier for the development card. It creates a
# one-shot random non-production blob directly in locked memory and requires an
# explicit interruption-risk acknowledgement before invoking the recoverable
# helper transaction. It is never part of the product or default build.
add_executable(usb_vault_vikey_development_blob_round_trip_probe
  EXCLUDE_FROM_ALL
  tools/vikey_development_session_probe/main.cpp
  tools/vikey_development_session_probe/random_test_blob.cpp
)
target_include_directories(
  usb_vault_vikey_development_blob_round_trip_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(
  usb_vault_vikey_development_blob_round_trip_probe PRIVATE
  USB_VAULT_VIKEY_BLOB_ROUND_TRIP_PROBE
)
target_link_libraries(
  usb_vault_vikey_development_blob_round_trip_probe PRIVATE
  usb_vault_vikey_development_credentials
  usb_vault_vikey_helper_client
  Bcrypt
)
usb_vault_configure_target(
  usb_vault_vikey_development_blob_round_trip_probe)

# Explicitly built read-only verifier for the complete formal development
# ICardStore chain over IPC v5. It never writes card data and is not part of
# the product or default build.
add_executable(usb_vault_vikey_formal_blob_read_probe EXCLUDE_FROM_ALL
  tools/vikey_development_session_probe/main.cpp
)
target_include_directories(usb_vault_vikey_formal_blob_read_probe PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(usb_vault_vikey_formal_blob_read_probe PRIVATE
  USB_VAULT_VIKEY_FORMAL_BLOB_READ_PROBE
)
target_link_libraries(usb_vault_vikey_formal_blob_read_probe PRIVATE
  usb_vault_vikey_development_credentials
  usb_vault_vikey_helper_blob_backend
)
usb_vault_configure_target(usb_vault_vikey_formal_blob_read_probe)

# One-purpose development utility for the current disposable P1 card. It
# refuses every media shape except the ambiguous 32-byte P1 prefix (or an
# already blank idempotent result), binds the probed hardware identity, and is
# never included in the product or default build.
add_executable(usb_vault_vikey_p1_card_reset EXCLUDE_FROM_ALL
  tools/vikey_p1_card_reset/main.cpp
)
target_include_directories(usb_vault_vikey_p1_card_reset PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(usb_vault_vikey_p1_card_reset PRIVATE
  usb_vault_vikey_development_credentials
  usb_vault_vikey_helper_client
)
usb_vault_configure_target(usb_vault_vikey_p1_card_reset)

# Explicit P1.7 acceptance support. The executable reads the development card
# once, derives fixed non-production fixture passwords in locked memory, and
# creates disposable local containers beside the development app. It is never
# part of the product or default build and requires an explicit runtime flag.
add_library(usb_vault_p1_7_development_fixture STATIC EXCLUDE_FROM_ALL
  tools/p1_7_development_fixture/development_container_fixture.cpp
)
target_include_directories(usb_vault_p1_7_development_fixture PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/tools/p1_7_development_fixture
)
target_link_libraries(usb_vault_p1_7_development_fixture PUBLIC
  usb_vault_core
)
usb_vault_configure_target(usb_vault_p1_7_development_fixture)

add_executable(usb_vault_p1_7_development_container_fixture EXCLUDE_FROM_ALL
  tools/p1_7_development_fixture/main.cpp
)
target_include_directories(
  usb_vault_p1_7_development_container_fixture PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(
  usb_vault_p1_7_development_container_fixture PRIVATE
  usb_vault_p1_7_development_fixture
  usb_vault_vikey_development_credentials
  usb_vault_vikey_helper_blob_backend
)
usb_vault_configure_target(
  usb_vault_p1_7_development_container_fixture)

# Explicit read-only-card P1.7 mount diagnostic. It uses only the disposable
# fixture passwords, never writes the card, and unmounts every successful
# diagnostic mount before returning. It is excluded from default builds.
add_executable(usb_vault_p1_7_mount_diagnostic EXCLUDE_FROM_ALL
  tools/p1_7_development_fixture/mount_diagnostic.cpp
)
target_include_directories(usb_vault_p1_7_mount_diagnostic PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(usb_vault_p1_7_mount_diagnostic PRIVATE
  usb_vault_p1_7_development_fixture
  usb_vault_vikey_development_credentials
  usb_vault_vikey_helper_blob_backend
)
usb_vault_configure_target(usb_vault_p1_7_mount_diagnostic)

add_executable(usb_vault_volume_creator WIN32
  tools/volume_creator/main.cpp
  src/volume_creation/creator_helper_engine.cpp
)

target_include_directories(usb_vault_volume_creator
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(usb_vault_volume_creator PRIVATE UNICODE _UNICODE)
target_link_libraries(usb_vault_volume_creator PRIVATE usb_vault_core Advapi32)
target_link_options(usb_vault_volume_creator PRIVATE
  "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'"
)
usb_vault_configure_target(usb_vault_volume_creator)

if(USB_VAULT_BUILD_UI)
  include(FetchContent)
  set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/out/dependencies")
  FetchContent_Declare(webview2_sdk
    URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4129.50/microsoft.web.webview2.1.0.4129.50.nupkg
    URL_HASH SHA256=D3934F482D484B89FB4825DF720C710664E1143A1E90F7B3A60794EF33F473D2
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(webview2_sdk)

  # Separate opt-in P2 application. It reuses the development credential and
  # network-enabled helper deployment boundary, but composes only the formal
  # 216-byte Card Record entry/login chain and the five-state product page.
  add_executable(usb_vault_vikey_p2_development_app WIN32 EXCLUDE_FROM_ALL
    tools/app/main.cpp
  )
  target_include_directories(usb_vault_vikey_p2_development_app PRIVATE
    "${webview2_sdk_SOURCE_DIR}/build/native/include"
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(usb_vault_vikey_p2_development_app PRIVATE
    usb_vault_core
    usb_vault_vikey_development_credentials
    usb_vault_vikey_helper_card_record_backend
    usb_vault_windows_hid_notifications
    usb_vault_windows_shell_container_chooser
    "${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    Ole32
    Shell32
    Shlwapi
  )
  target_compile_definitions(usb_vault_vikey_p2_development_app PRIVATE
    UNICODE
    _UNICODE
    USB_VAULT_VIKEY_P2_DEVELOPMENT_APP
  )

  if(USB_VAULT_FORMAT_DLL)
    add_custom_target(usb_vault_p2_format_backend_deployment
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${USB_VAULT_FORMAT_DLL}"
              "$<TARGET_FILE_DIR:usb_vault_volume_creator>/VeraCryptFormat.dll"
      VERBATIM
    )
  else()
    add_custom_target(usb_vault_p2_format_backend_deployment
      COMMAND ${CMAKE_COMMAND} -E echo
              "P2 development app requires -DUSB_VAULT_FORMAT_DLL=<patched VeraCryptFormat.dll>"
      COMMAND ${CMAKE_COMMAND} -E false
      VERBATIM
    )
  endif()
  add_dependencies(usb_vault_vikey_p2_development_app
    usb_vault_p2_format_backend_deployment
    usb_vault_vikey_helper
    usb_vault_volume_creator
  )
  usb_vault_configure_target(usb_vault_vikey_p2_development_app)

  add_custom_command(TARGET usb_vault_vikey_p2_development_app POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/card_session.html"
            "$<TARGET_FILE_DIR:usb_vault_vikey_p2_development_app>/card_session.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/ui/card_session.js"
            "$<TARGET_FILE_DIR:usb_vault_vikey_p2_development_app>/card_session.js"
    VERBATIM
  )
  set_property(TARGET usb_vault_vikey_p2_development_app APPEND PROPERTY
    LINK_DEPENDS
    "${CMAKE_SOURCE_DIR}/ui/card_session.html"
    "${CMAKE_SOURCE_DIR}/ui/card_session.js"
  )

  # Explicit aggregation point for the internal P2 development package. The
  # staging script copies only these reviewed runtime outputs and never walks a
  # build directory for arbitrary DLLs or debug artifacts.
  add_custom_target(usb_vault_p2_development_package_binaries)
  add_dependencies(usb_vault_p2_development_package_binaries
    usb_vault_vikey_p2_development_app
    usb_vault_vikey_development_credentials_config
  )
endif()

if(USB_VAULT_FORMAT_DLL)
  if(NOT EXISTS "${USB_VAULT_FORMAT_DLL}")
    message(FATAL_ERROR "USB_VAULT_FORMAT_DLL does not exist: ${USB_VAULT_FORMAT_DLL}")
  endif()
  add_custom_command(TARGET usb_vault_volume_creator POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${USB_VAULT_FORMAT_DLL}"
            "$<TARGET_FILE_DIR:usb_vault_volume_creator>/VeraCryptFormat.dll"
    VERBATIM
  )
endif()

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
