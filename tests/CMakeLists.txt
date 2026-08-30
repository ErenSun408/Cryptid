add_executable(secure_buffer_tests
  secure_buffer_tests.cpp
)

target_link_libraries(secure_buffer_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(secure_buffer_tests)

add_test(NAME secure_buffer_tests COMMAND secure_buffer_tests)

add_executable(kdf_v1_tests
  kdf_v1_tests.cpp
)

target_link_libraries(kdf_v1_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(kdf_v1_tests)

add_test(NAME kdf_v1_tests COMMAND kdf_v1_tests)

add_executable(card_record_journal_v1_tests
  card_record_journal_v1_tests.cpp
)

target_include_directories(card_record_journal_v1_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(card_record_journal_v1_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_record_journal_v1_tests)

add_test(NAME card_record_journal_v1_tests
  COMMAND card_record_journal_v1_tests)

add_executable(development_container_fixture_tests
  development_container_fixture_tests.cpp
)

target_link_libraries(development_container_fixture_tests PRIVATE
  usb_vault_p1_7_development_fixture
)

usb_vault_configure_target(development_container_fixture_tests)

add_test(NAME development_container_fixture_tests
  COMMAND development_container_fixture_tests)

add_executable(volume_provider_tests
  volume_provider_tests.cpp
)

target_include_directories(volume_provider_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(volume_provider_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(volume_provider_tests)

add_test(NAME volume_provider_tests COMMAND volume_provider_tests)

add_executable(mount_orchestrator_tests
  mount_orchestrator_tests.cpp
)

target_include_directories(mount_orchestrator_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(mount_orchestrator_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(mount_orchestrator_tests)

add_test(NAME mount_orchestrator_tests COMMAND mount_orchestrator_tests)

add_executable(creator_protocol_tests
  creator_protocol_tests.cpp
)

target_include_directories(creator_protocol_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(creator_protocol_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(creator_protocol_tests)

add_test(NAME creator_protocol_tests COMMAND creator_protocol_tests)

add_executable(card_store_contract_tests
  card_store_contract_tests.cpp
)

target_link_libraries(card_store_contract_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(card_store_contract_tests)

add_test(NAME card_store_contract_tests COMMAND card_store_contract_tests)

add_executable(card_record_v1_tests
  card_record_v1_tests.cpp
)

target_link_libraries(card_record_v1_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(card_record_v1_tests)

add_test(NAME card_record_v1_tests COMMAND card_record_v1_tests)

add_executable(card_record_service_tests
  card_record_service_tests.cpp
)

target_link_libraries(card_record_service_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_record_service_tests)

add_test(NAME card_record_service_tests
  COMMAND card_record_service_tests)

add_executable(confirmed_password_tests
  confirmed_password_tests.cpp
)

target_link_libraries(confirmed_password_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(confirmed_password_tests)

add_test(NAME confirmed_password_tests
  COMMAND confirmed_password_tests)

add_executable(card_initialization_service_tests
  card_initialization_service_tests.cpp
)

target_link_libraries(card_initialization_service_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_initialization_service_tests)

add_test(NAME card_initialization_service_tests
  COMMAND card_initialization_service_tests)

add_executable(fixed_blob_card_store_tests
  fixed_blob_card_store_tests.cpp
)

target_link_libraries(fixed_blob_card_store_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(fixed_blob_card_store_tests)

add_test(NAME fixed_blob_card_store_tests COMMAND fixed_blob_card_store_tests)

add_executable(vikey_card_store_tests
  vikey_card_store_tests.cpp
)

target_include_directories(vikey_card_store_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_card_store_tests PRIVATE
  usb_vault_vikey_card_store
)

usb_vault_configure_target(vikey_card_store_tests)

add_test(NAME vikey_card_store_tests COMMAND vikey_card_store_tests)

add_executable(vikey_card_record_store_tests
  vikey_card_record_store_tests.cpp
)

target_include_directories(vikey_card_record_store_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_card_record_store_tests PRIVATE
  usb_vault_vikey_card_store
)

usb_vault_configure_target(vikey_card_record_store_tests)

add_test(NAME vikey_card_record_store_tests
  COMMAND vikey_card_record_store_tests)

add_executable(vikey_development_credentials_tests
  vikey_development_credentials_tests.cpp
)

target_include_directories(vikey_development_credentials_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_development_credentials_tests PRIVATE
  usb_vault_vikey_development_credentials
)

usb_vault_configure_target(vikey_development_credentials_tests)

add_test(NAME vikey_development_credentials_tests
  COMMAND vikey_development_credentials_tests)

add_executable(vikey_development_app_config_tests
  vikey_development_app_config_tests.cpp
)

target_include_directories(vikey_development_app_config_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_development_app_config_tests PRIVATE
  usb_vault_vikey_development_credentials
)

usb_vault_configure_target(vikey_development_app_config_tests)

add_test(NAME vikey_development_app_config_tests
  COMMAND vikey_development_app_config_tests)

add_executable(vikey_module_tests
  vikey_module_tests.cpp
)

target_include_directories(vikey_module_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_module_tests
  PRIVATE
    usb_vault_vikey_sdk
)

usb_vault_configure_target(vikey_module_tests)

add_test(NAME vikey_module_tests COMMAND vikey_module_tests)

add_executable(vikey_session_tests
  vikey_session_tests.cpp
)

target_include_directories(vikey_session_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_session_tests
  PRIVATE
    usb_vault_vikey_sdk
)

usb_vault_configure_target(vikey_session_tests)

add_test(NAME vikey_session_tests COMMAND vikey_session_tests)

add_executable(vikey_card_record_io_tests
  vikey_card_record_io_tests.cpp
)

target_include_directories(vikey_card_record_io_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_card_record_io_tests PRIVATE
  usb_vault_vikey_sdk
)

usb_vault_configure_target(vikey_card_record_io_tests)

add_test(NAME vikey_card_record_io_tests
  COMMAND vikey_card_record_io_tests)

add_executable(vikey_development_test_blob_tests
  vikey_development_test_blob_tests.cpp
  ${PROJECT_SOURCE_DIR}/tools/vikey_development_session_probe/random_test_blob.cpp
)

target_include_directories(vikey_development_test_blob_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/tools/vikey_development_session_probe
)

target_link_libraries(vikey_development_test_blob_tests PRIVATE
  usb_vault_core
  Bcrypt
)

usb_vault_configure_target(vikey_development_test_blob_tests)

add_test(NAME vikey_development_test_blob_tests
  COMMAND vikey_development_test_blob_tests)

add_executable(vikey_artifact_tests
  vikey_artifact_tests.cpp
)

target_include_directories(vikey_artifact_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_artifact_tests
  PRIVATE
    usb_vault_vikey_sdk
)

usb_vault_configure_target(vikey_artifact_tests)

add_test(NAME vikey_artifact_tests COMMAND vikey_artifact_tests)

add_executable(vikey_helper_protocol_tests
  vikey_helper_protocol_tests.cpp
)

target_include_directories(vikey_helper_protocol_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_helper_protocol_tests PRIVATE
  usb_vault_vikey_helper_protocol
)

usb_vault_configure_target(vikey_helper_protocol_tests)

add_test(NAME vikey_helper_protocol_tests COMMAND vikey_helper_protocol_tests)

add_executable(vikey_helper_stall_fixture WIN32
  vikey_helper_stall_fixture.cpp
)

set_property(TARGET vikey_helper_stall_fixture PROPERTY MSVC_RUNTIME_LIBRARY
  "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

usb_vault_configure_target(vikey_helper_stall_fixture)

add_executable(vikey_helper_network_fixture WIN32
  vikey_helper_network_fixture.cpp
  vikey_network_probe.cpp
  ${PROJECT_SOURCE_DIR}/src/card/vikey/vikey_helper_protocol.cpp
  ${PROJECT_SOURCE_DIR}/src/card/card_error.cpp
  ${PROJECT_SOURCE_DIR}/src/security/secure_buffer.cpp
)

target_include_directories(vikey_helper_network_fixture PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(vikey_helper_network_fixture PRIVATE
  UNICODE
  _UNICODE
)
set_property(TARGET vikey_helper_network_fixture PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_link_libraries(vikey_helper_network_fixture PRIVATE Ws2_32)
usb_vault_configure_target(vikey_helper_network_fixture)

add_executable(vikey_helper_network_enabled_fixture WIN32
  vikey_helper_network_fixture.cpp
  vikey_network_probe.cpp
  ${PROJECT_SOURCE_DIR}/src/card/vikey/vikey_helper_protocol.cpp
  ${PROJECT_SOURCE_DIR}/src/card/card_error.cpp
  ${PROJECT_SOURCE_DIR}/src/security/secure_buffer.cpp
)

target_include_directories(vikey_helper_network_enabled_fixture PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(vikey_helper_network_enabled_fixture PRIVATE
  UNICODE
  _UNICODE
  USB_VAULT_EXPECT_NETWORK_ACCESS
)
set_property(TARGET vikey_helper_network_enabled_fixture PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
target_link_libraries(vikey_helper_network_enabled_fixture PRIVATE Ws2_32)
usb_vault_configure_target(vikey_helper_network_enabled_fixture)

add_library(vikey_vendor_runtime_fixture SHARED
  vikey_vendor_runtime_fixture.cpp
)
set_target_properties(vikey_vendor_runtime_fixture PROPERTIES
  OUTPUT_NAME ViKey
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
usb_vault_configure_target(vikey_vendor_runtime_fixture)
target_link_libraries(vikey_vendor_runtime_fixture PRIVATE Bcrypt)

add_executable(vikey_helper_process_tests
  vikey_helper_process_tests.cpp
  vikey_network_probe.cpp
)

add_dependencies(vikey_helper_process_tests
  usb_vault_vikey_helper
  vikey_helper_network_fixture
  vikey_helper_network_enabled_fixture
  vikey_helper_stall_fixture
  vikey_vendor_runtime_fixture
)

target_include_directories(vikey_helper_process_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_helper_process_tests PRIVATE
  usb_vault_vikey_helper_blob_backend
  usb_vault_vikey_helper_card_record_backend
  usb_vault_vikey_helper_client
  Ws2_32
)

usb_vault_configure_target(vikey_helper_process_tests)

add_test(
  NAME vikey_helper_process_tests
  COMMAND vikey_helper_process_tests
          $<TARGET_FILE:usb_vault_vikey_helper>
          $<TARGET_FILE:vikey_helper_network_fixture>
          $<TARGET_FILE:vikey_helper_network_enabled_fixture>
          $<TARGET_FILE:vikey_helper_stall_fixture>
          $<TARGET_FILE:vikey_vendor_runtime_fixture>
)
set_tests_properties(vikey_helper_process_tests PROPERTIES
  RUN_SERIAL TRUE
  SKIP_RETURN_CODE 77
)

add_executable(vikey_helper_hid_compatibility_tests
  vikey_helper_hid_compatibility_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/card/vikey/vikey_hid_probe.cpp
)

add_dependencies(vikey_helper_hid_compatibility_tests
  usb_vault_vikey_helper
)

target_include_directories(vikey_helper_hid_compatibility_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_helper_hid_compatibility_tests PRIVATE
  usb_vault_vikey_helper_client
  Bcrypt
  Hid
  Setupapi
)

usb_vault_configure_target(vikey_helper_hid_compatibility_tests)

add_test(
  NAME vikey_helper_hid_compatibility_tests
  COMMAND vikey_helper_hid_compatibility_tests
          $<TARGET_FILE:usb_vault_vikey_helper>
)
set_tests_properties(vikey_helper_hid_compatibility_tests PROPERTIES
  RUN_SERIAL TRUE
  SKIP_RETURN_CODE 77
)

add_executable(application_session_tests
  application_session_tests.cpp
)

target_include_directories(application_session_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(application_session_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(application_session_tests)

add_test(NAME application_session_tests COMMAND application_session_tests)

add_executable(card_presence_controller_tests
  card_presence_controller_tests.cpp
)

target_link_libraries(card_presence_controller_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(card_presence_controller_tests)

add_test(NAME card_presence_controller_tests
  COMMAND card_presence_controller_tests)

add_executable(hid_card_device_notifications_tests
  hid_card_device_notifications_tests.cpp
)

target_link_libraries(hid_card_device_notifications_tests
  PRIVATE
    usb_vault_windows_hid_notifications
)

usb_vault_configure_target(hid_card_device_notifications_tests)

add_test(NAME hid_card_device_notifications_tests
  COMMAND hid_card_device_notifications_tests)

add_executable(password_ingress_tests
  password_ingress_tests.cpp
)

target_link_libraries(password_ingress_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(password_ingress_tests)

add_test(NAME password_ingress_tests COMMAND password_ingress_tests)

add_executable(ui_session_controller_tests
  ui_session_controller_tests.cpp
)

target_link_libraries(ui_session_controller_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(ui_session_controller_tests)

add_test(NAME ui_session_controller_tests COMMAND ui_session_controller_tests)

add_executable(ui_close_guard_tests
  ui_close_guard_tests.cpp
)

target_link_libraries(ui_close_guard_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(ui_close_guard_tests)

add_test(NAME ui_close_guard_tests COMMAND ui_close_guard_tests)

add_executable(ui_page_contract_tests
  ui_page_contract_tests.cpp
)

target_compile_definitions(ui_page_contract_tests PRIVATE
  USB_VAULT_UI_PAGE="${PROJECT_SOURCE_DIR}/ui/index.html"
  USB_VAULT_UI_SCRIPT="${PROJECT_SOURCE_DIR}/ui/session.js"
)

usb_vault_configure_target(ui_page_contract_tests)

add_test(NAME ui_page_contract_tests COMMAND ui_page_contract_tests)

add_executable(webview_password_array_tests
  webview_password_array_tests.cpp
)

target_link_libraries(webview_password_array_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(webview_password_array_tests)

add_test(NAME webview_password_array_tests COMMAND webview_password_array_tests)

add_executable(webview_host_object_tests
  webview_host_object_tests.cpp
)

target_link_libraries(webview_host_object_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(webview_host_object_tests)

add_test(NAME webview_host_object_tests COMMAND webview_host_object_tests)

if(USB_VAULT_BUILD_UI)
  add_executable(webview_typed_array_integration_tests WIN32 EXCLUDE_FROM_ALL
    webview_typed_array_integration_tests.cpp
  )

  target_include_directories(webview_typed_array_integration_tests PRIVATE
    "${webview2_sdk_SOURCE_DIR}/build/native/include"
  )

  target_link_libraries(webview_typed_array_integration_tests PRIVATE
    usb_vault_core
    "${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    Ole32
    Shlwapi
  )

  usb_vault_configure_target(webview_typed_array_integration_tests)

  add_custom_command(TARGET webview_typed_array_integration_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/webview_session_integration.html"
            "$<TARGET_FILE_DIR:webview_typed_array_integration_tests>/webview_session_integration.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/ui/session.js"
            "$<TARGET_FILE_DIR:webview_typed_array_integration_tests>/session.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/webview_typed_array_integration.js"
            "$<TARGET_FILE_DIR:webview_typed_array_integration_tests>/webview_typed_array_integration.js"
    VERBATIM
  )
  set_property(TARGET webview_typed_array_integration_tests APPEND PROPERTY
    LINK_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/webview_session_integration.html"
      "${CMAKE_CURRENT_SOURCE_DIR}/webview_typed_array_integration.js"
      "${PROJECT_SOURCE_DIR}/ui/session.js"
  )
endif()

add_executable(secure_buffer_copy_compile_fail EXCLUDE_FROM_ALL
  secure_buffer_copy_compile_fail.cpp
)

target_link_libraries(secure_buffer_copy_compile_fail
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(secure_buffer_copy_compile_fail)

add_test(
  NAME secure_buffer_copy_is_rejected
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
          --target secure_buffer_copy_compile_fail
)

set_tests_properties(secure_buffer_copy_is_rejected PROPERTIES WILL_FAIL TRUE)

if(USB_VAULT_ENABLE_INTEGRATION_TESTS)
  add_executable(volume_provider_integration_tests
    volume_provider_integration_tests.cpp
  )

  target_link_libraries(volume_provider_integration_tests
    PRIVATE
      usb_vault_core
  )

  usb_vault_configure_target(volume_provider_integration_tests)

  add_test(
    NAME volume_provider_integration_tests
    COMMAND volume_provider_integration_tests
            ${PROJECT_SOURCE_DIR}/out/vendor/kdf_v1_fixture.hc
  )
  set_tests_properties(volume_provider_integration_tests PROPERTIES
    SKIP_RETURN_CODE 77
    RUN_SERIAL TRUE
  )

  add_executable(mount_orchestrator_integration_tests
    mount_orchestrator_integration_tests.cpp
  )

  target_link_libraries(mount_orchestrator_integration_tests
    PRIVATE
      usb_vault_core
  )

  usb_vault_configure_target(mount_orchestrator_integration_tests)

  add_test(
    NAME mount_orchestrator_integration_tests
    COMMAND mount_orchestrator_integration_tests
            ${PROJECT_SOURCE_DIR}/out/vendor/p1_3_a.hc
            ${PROJECT_SOURCE_DIR}/out/vendor/p1_3_b.hc
  )
  set_tests_properties(mount_orchestrator_integration_tests PROPERTIES
    SKIP_RETURN_CODE 77
    RUN_SERIAL TRUE
  )

  add_executable(volume_creator_integration_tests
    volume_creator_integration_tests.cpp
  )

  target_link_libraries(volume_creator_integration_tests
    PRIVATE
      usb_vault_core
  )

  usb_vault_configure_target(volume_creator_integration_tests)

  add_test(
    NAME volume_creator_integration_tests
    COMMAND volume_creator_integration_tests
            $<TARGET_FILE:usb_vault_volume_creator>
            ${PROJECT_SOURCE_DIR}/out/vendor/p1_4_a.hc
            ${PROJECT_SOURCE_DIR}/out/vendor/p1_4_b.hc
            ${PROJECT_SOURCE_DIR}/spike/vcspike.exe
  )
  set_tests_properties(volume_creator_integration_tests PROPERTIES
    SKIP_RETURN_CODE 77
    RUN_SERIAL TRUE
  )
endif()
