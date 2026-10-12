add_test(NAME release_log_policy_tests
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/release_log_policy_tests.cmake")

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

add_executable(volume_space_tests
  volume_space_tests.cpp
)

target_include_directories(volume_space_tests
  PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(volume_space_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(volume_space_tests)

add_test(NAME volume_space_tests COMMAND volume_space_tests)

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
  ${PROJECT_SOURCE_DIR}/src/volume_creation/creator_helper_engine.cpp
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

add_executable(creator_helper_log_tests creator_helper_log_tests.cpp)
target_include_directories(creator_helper_log_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_dependencies(creator_helper_log_tests usb_vault_volume_creator)
usb_vault_configure_target(creator_helper_log_tests)
add_test(NAME creator_helper_log_tests
         COMMAND creator_helper_log_tests $<TARGET_FILE:usb_vault_volume_creator>)

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

add_executable(card_entry_router_tests
  card_entry_router_tests.cpp
)

target_link_libraries(card_entry_router_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_entry_router_tests)

add_test(NAME card_entry_router_tests
  COMMAND card_entry_router_tests)

add_executable(card_entry_context_controller_tests
  card_entry_context_controller_tests.cpp
)

target_link_libraries(card_entry_context_controller_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_entry_context_controller_tests)

add_test(NAME card_entry_context_controller_tests
  COMMAND card_entry_context_controller_tests)

add_executable(startup_failure_tests startup_failure_tests.cpp)
target_include_directories(startup_failure_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(startup_failure_tests PRIVATE usb_vault_core)
usb_vault_configure_target(startup_failure_tests)
add_test(NAME startup_failure_tests COMMAND startup_failure_tests)

add_executable(webview_startup_state_tests webview_startup_state_tests.cpp)
target_include_directories(webview_startup_state_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
usb_vault_configure_target(webview_startup_state_tests)
add_test(NAME webview_startup_state_tests COMMAND webview_startup_state_tests)

add_executable(startup_log_tests startup_log_tests.cpp)
target_include_directories(startup_log_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
usb_vault_configure_target(startup_log_tests)
add_test(NAME startup_log_tests COMMAND startup_log_tests)

add_executable(process_security_log_matrix_tests process_security_log_matrix_tests.cpp)
target_include_directories(process_security_log_matrix_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(process_security_log_matrix_tests PRIVATE usb_vault_core)
usb_vault_configure_target(process_security_log_matrix_tests)
add_test(NAME process_security_log_matrix_tests COMMAND process_security_log_matrix_tests)


add_executable(card_entry_initialization_controller_tests
  card_entry_initialization_controller_tests.cpp
)

target_link_libraries(card_entry_initialization_controller_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_entry_initialization_controller_tests)

add_test(NAME card_entry_initialization_controller_tests
  COMMAND card_entry_initialization_controller_tests)

add_executable(card_entry_login_controller_tests
  card_entry_login_controller_tests.cpp
)

target_link_libraries(card_entry_login_controller_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_entry_login_controller_tests)

add_test(NAME card_entry_login_controller_tests
  COMMAND card_entry_login_controller_tests)

add_executable(card_authentication_session_tests
  card_authentication_session_tests.cpp
)

target_link_libraries(card_authentication_session_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_authentication_session_tests)

add_test(NAME card_authentication_session_tests
  COMMAND card_authentication_session_tests)

add_executable(card_login_mount_session_tests
  card_login_mount_session_tests.cpp
)

target_include_directories(card_login_mount_session_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(card_login_mount_session_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_login_mount_session_tests)

add_test(NAME card_login_mount_session_tests
  COMMAND card_login_mount_session_tests)

add_executable(product_settings_v1_tests
  product_settings_v1_tests.cpp
)

target_link_libraries(product_settings_v1_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(product_settings_v1_tests)

add_test(NAME product_settings_v1_tests
  COMMAND product_settings_v1_tests)

add_executable(container_index_store_v1_tests
  container_index_store_v1_tests.cpp
)

target_link_libraries(container_index_store_v1_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(container_index_store_v1_tests)

add_test(NAME container_index_store_v1_tests
  COMMAND container_index_store_v1_tests)

add_executable(container_index_v1_tests
  container_index_v1_tests.cpp
)

target_link_libraries(container_index_v1_tests PRIVATE
  usb_vault_core
  Bcrypt
)

usb_vault_configure_target(container_index_v1_tests)

add_test(NAME container_index_v1_tests
  COMMAND container_index_v1_tests)

add_executable(container_index_view_v1_tests
  container_index_view_v1_tests.cpp
)

target_link_libraries(container_index_view_v1_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(container_index_view_v1_tests)

add_test(NAME container_index_view_v1_tests
  COMMAND container_index_view_v1_tests)

add_executable(volume_file_operations_tests
  volume_file_operations_tests.cpp
)

target_link_libraries(volume_file_operations_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(volume_file_operations_tests)

add_test(NAME volume_file_operations_tests
  COMMAND volume_file_operations_tests)

add_executable(volume_directory_listing_tests
  volume_directory_listing_tests.cpp
)

target_link_libraries(volume_directory_listing_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(volume_directory_listing_tests)

add_test(NAME volume_directory_listing_tests
  COMMAND volume_directory_listing_tests)

add_executable(volume_entry_path_tests
  volume_entry_path_tests.cpp
)

target_link_libraries(volume_entry_path_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(volume_entry_path_tests)

add_test(NAME volume_entry_path_tests
  COMMAND volume_entry_path_tests)

add_executable(container_target_path_tests
  container_target_path_tests.cpp
)

target_link_libraries(container_target_path_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(container_target_path_tests)

add_test(NAME container_target_path_tests
  COMMAND container_target_path_tests)

add_executable(card_product_ui_session_controller_tests
  card_product_ui_session_controller_tests.cpp
)

target_link_libraries(card_product_ui_session_controller_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_product_ui_session_controller_tests)

add_test(NAME card_product_ui_session_controller_tests
  COMMAND card_product_ui_session_controller_tests)

add_executable(card_product_webview_host_object_tests
  card_product_webview_host_object_tests.cpp
)

target_link_libraries(card_product_webview_host_object_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_product_webview_host_object_tests)

add_test(NAME card_product_webview_host_object_tests
  COMMAND card_product_webview_host_object_tests)

add_executable(external_drop_target_tests
  ${PROJECT_SOURCE_DIR}/tools/app/usb_vault_app.manifest
  external_drop_target_tests.cpp
)

target_link_libraries(external_drop_target_tests PRIVATE
  usb_vault_core
  Ole32
  Shell32
)

usb_vault_configure_target(external_drop_target_tests)

add_test(NAME external_drop_target_tests
  COMMAND external_drop_target_tests)

add_executable(card_configuration_webview_host_object_tests
  card_configuration_webview_host_object_tests.cpp
)

target_link_libraries(card_configuration_webview_host_object_tests PRIVATE
  usb_vault_core
  OleAut32
)

usb_vault_configure_target(card_configuration_webview_host_object_tests)

add_test(NAME card_configuration_webview_host_object_tests
  COMMAND card_configuration_webview_host_object_tests)

add_executable(card_device_lifecycle_controller_tests
  card_device_lifecycle_controller_tests.cpp
)

target_link_libraries(card_device_lifecycle_controller_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_device_lifecycle_controller_tests)

add_test(NAME card_device_lifecycle_controller_tests
  COMMAND card_device_lifecycle_controller_tests)

add_executable(vikey_card_insertion_context_tests
  vikey_card_insertion_context_tests.cpp
)

target_include_directories(vikey_card_insertion_context_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_card_insertion_context_tests PRIVATE
  usb_vault_vikey_helper_card_record_backend
)

usb_vault_configure_target(vikey_card_insertion_context_tests)

add_test(NAME vikey_card_insertion_context_tests
  COMMAND vikey_card_insertion_context_tests)

add_executable(vikey_card_initialization_target_tests
  vikey_card_initialization_target_tests.cpp
)

target_include_directories(vikey_card_initialization_target_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_card_initialization_target_tests PRIVATE
  usb_vault_vikey_helper_card_record_backend
)

usb_vault_configure_target(vikey_card_initialization_target_tests)

add_test(NAME vikey_card_initialization_target_tests
  COMMAND vikey_card_initialization_target_tests)

add_executable(vikey_card_login_target_tests
  vikey_card_login_target_tests.cpp
)

target_include_directories(vikey_card_login_target_tests PRIVATE
  ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(vikey_card_login_target_tests PRIVATE
  usb_vault_vikey_helper_card_record_backend
)

usb_vault_configure_target(vikey_card_login_target_tests)

add_test(NAME vikey_card_login_target_tests
  COMMAND vikey_card_login_target_tests)

add_executable(confirmed_password_tests
  confirmed_password_tests.cpp
)

target_link_libraries(confirmed_password_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(confirmed_password_tests)

add_test(NAME confirmed_password_tests
  COMMAND confirmed_password_tests)

add_executable(offline_card_password_policy_tests
  offline_card_password_policy_tests.cpp
)

target_link_libraries(offline_card_password_policy_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(offline_card_password_policy_tests)

add_test(NAME offline_card_password_policy_tests
  COMMAND offline_card_password_policy_tests)

add_executable(password_normalization_tests
  password_normalization_tests.cpp
)

target_link_libraries(password_normalization_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(password_normalization_tests)

add_test(NAME password_normalization_tests
  COMMAND password_normalization_tests)

add_executable(card_initialization_service_tests
  card_initialization_service_tests.cpp
)

target_link_libraries(card_initialization_service_tests PRIVATE
  usb_vault_core
)

usb_vault_configure_target(card_initialization_service_tests)

add_test(NAME card_initialization_service_tests
  COMMAND card_initialization_service_tests)

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

add_executable(configuration_installer_tests
  configuration_installer_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/platform/windows/vikey_configuration/driver_deployment.cpp
  ${PROJECT_SOURCE_DIR}/src/platform/windows/vikey_configuration/driver_cleanup.cpp
  ${PROJECT_SOURCE_DIR}/src/platform/windows/vikey_configuration/installers.cpp)
target_include_directories(configuration_installer_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(configuration_installer_tests PRIVATE
  usb_vault_core usb_vault_vikey_development_credentials usb_vault_verified_local_artifact Shell32 Wintrust Advapi32)
usb_vault_configure_target(configuration_installer_tests)
add_test(NAME configuration_installer_tests COMMAND configuration_installer_tests)

add_executable(webview_cleanup_tests
  webview_cleanup_tests.cpp
  ${PROJECT_SOURCE_DIR}/src/platform/windows/vikey_configuration/webview_cleanup.cpp)
target_include_directories(webview_cleanup_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(webview_cleanup_tests PRIVATE Shell32 Ole32 Wintrust Crypt32 Advapi32)
usb_vault_configure_target(webview_cleanup_tests)
add_test(NAME webview_cleanup_tests COMMAND webview_cleanup_tests)

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

add_executable(helper_environment_tests helper_environment_tests.cpp)
target_include_directories(helper_environment_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(helper_environment_tests PRIVATE usb_vault_vikey_helper_protocol)
usb_vault_configure_target(helper_environment_tests)
add_dependencies(helper_environment_tests usb_vault_vikey_helper)
add_test(NAME helper_environment_tests COMMAND helper_environment_tests $<TARGET_FILE:usb_vault_vikey_helper>)


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

add_executable(ui_close_guard_tests
  ui_close_guard_tests.cpp
)

target_link_libraries(ui_close_guard_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(ui_close_guard_tests)

add_test(NAME ui_close_guard_tests COMMAND ui_close_guard_tests)

add_executable(window_tray_tests window_tray_tests.cpp)
target_link_libraries(window_tray_tests PRIVATE user32 shell32)
usb_vault_configure_target(window_tray_tests)
add_test(NAME window_tray_tests COMMAND window_tray_tests)

add_executable(card_product_ui_page_contract_tests
  card_product_ui_page_contract_tests.cpp
)

target_include_directories(card_product_ui_page_contract_tests PRIVATE
  "${PROJECT_SOURCE_DIR}/include"
)

target_compile_definitions(card_product_ui_page_contract_tests PRIVATE
  USB_VAULT_CARD_PRODUCT_UI_PAGE="${PROJECT_SOURCE_DIR}/ui/card_session.html"
  USB_VAULT_CARD_PRODUCT_UI_SCRIPT="${PROJECT_SOURCE_DIR}/ui/card_session.js"
  USB_VAULT_CARD_PRODUCT_UI_FIXTURE_PAGE="${CMAKE_CURRENT_SOURCE_DIR}/card_product_webview_integration.html"
  USB_VAULT_PRODUCT_VERSION="${PROJECT_VERSION}"
)

usb_vault_configure_target(card_product_ui_page_contract_tests)

add_test(NAME card_product_ui_page_contract_tests
  COMMAND card_product_ui_page_contract_tests)

add_executable(webview_password_array_tests
  webview_password_array_tests.cpp
)

target_link_libraries(webview_password_array_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(webview_password_array_tests)

add_test(NAME webview_password_array_tests COMMAND webview_password_array_tests)

if(USB_VAULT_BUILD_UI)
  add_executable(card_product_webview_integration_tests WIN32 EXCLUDE_FROM_ALL
    card_product_webview_integration_tests.cpp
  )

  target_include_directories(card_product_webview_integration_tests PRIVATE
    "${webview2_sdk_SOURCE_DIR}/build/native/include"
  )

  target_link_libraries(card_product_webview_integration_tests PRIVATE
    usb_vault_core
    "${webview2_sdk_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib"
    Ole32
    Shlwapi
  )

  usb_vault_configure_target(card_product_webview_integration_tests)

  add_custom_command(TARGET card_product_webview_integration_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/card_product_webview_integration.html"
            "$<TARGET_FILE_DIR:card_product_webview_integration_tests>/card_product_webview_integration.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/ui/card_session.js"
            "$<TARGET_FILE_DIR:card_product_webview_integration_tests>/card_session.js"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/card_product_webview_integration.js"
            "$<TARGET_FILE_DIR:card_product_webview_integration_tests>/card_product_webview_integration.js"
    VERBATIM
  )
  set_property(TARGET card_product_webview_integration_tests APPEND PROPERTY
    LINK_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/card_product_webview_integration.html"
      "${CMAKE_CURRENT_SOURCE_DIR}/card_product_webview_integration.js"
      "${PROJECT_SOURCE_DIR}/ui/card_session.js"
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
  add_executable(decoy_allocation_behavior_tests EXCLUDE_FROM_ALL
    decoy_allocation_behavior_tests.cpp)
  target_include_directories(decoy_allocation_behavior_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(decoy_allocation_behavior_tests PRIVATE usb_vault_core bcrypt)
  usb_vault_configure_target(decoy_allocation_behavior_tests)
  # Explicit candidate DLL + disposable fixture; never runs against user data.
  add_executable(hidden_geometry_integration_tests EXCLUDE_FROM_ALL
    hidden_geometry_integration_tests.cpp)
  target_include_directories(hidden_geometry_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(hidden_geometry_integration_tests PRIVATE usb_vault_core)
  usb_vault_configure_target(hidden_geometry_integration_tests)
  # Explicit opt-in executable only: creates and deliberately fills a NEW
  # disposable hidden-volume fixture. Never run implicitly through CTest.
  add_executable(decoy_maintenance_integration_tests EXCLUDE_FROM_ALL
    decoy_maintenance_integration_tests.cpp)
  target_link_libraries(decoy_maintenance_integration_tests PRIVATE usb_vault_core)
  usb_vault_configure_target(decoy_maintenance_integration_tests)

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

add_executable(configuration_card_failure_tests configuration_card_failure_tests.cpp)
target_include_directories(configuration_card_failure_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(configuration_card_failure_tests PRIVATE usb_vault_vikey_sdk)
usb_vault_configure_target(configuration_card_failure_tests)
add_test(NAME configuration_card_failure_tests COMMAND configuration_card_failure_tests)

add_executable(fat32_safe_space_tests fat32_safe_space_tests.cpp)
target_include_directories(fat32_safe_space_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(fat32_safe_space_tests PRIVATE usb_vault_core)
usb_vault_configure_target(fat32_safe_space_tests)
add_test(NAME fat32_safe_space_tests COMMAND fat32_safe_space_tests)

add_executable(fat32_allocation_plan_tests fat32_allocation_plan_tests.cpp)
target_include_directories(fat32_allocation_plan_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(fat32_allocation_plan_tests PRIVATE usb_vault_core)
usb_vault_configure_target(fat32_allocation_plan_tests)
add_test(NAME fat32_allocation_plan_tests COMMAND fat32_allocation_plan_tests)

add_executable(fat32_file_allocation_tests fat32_file_allocation_tests.cpp)
target_include_directories(fat32_file_allocation_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(fat32_file_allocation_tests PRIVATE usb_vault_core)
usb_vault_configure_target(fat32_file_allocation_tests)
add_test(NAME fat32_file_allocation_tests COMMAND fat32_file_allocation_tests)

add_executable(import_unique_name_tests import_unique_name_tests.cpp)
target_include_directories(import_unique_name_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
usb_vault_configure_target(import_unique_name_tests)
add_test(NAME import_unique_name_tests COMMAND import_unique_name_tests)

add_executable(card_reset_confirmation_tests card_reset_confirmation_tests.cpp)
target_include_directories(card_reset_confirmation_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
usb_vault_configure_target(card_reset_confirmation_tests)
add_test(NAME card_reset_confirmation_tests COMMAND card_reset_confirmation_tests)

add_executable(card_operation_lease_tests card_operation_lease_tests.cpp)
target_include_directories(card_operation_lease_tests PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
usb_vault_configure_target(card_operation_lease_tests)
add_test(NAME card_operation_lease_tests COMMAND card_operation_lease_tests)

add_executable(partition_catalog_tests partition_catalog_tests.cpp)
target_include_directories(partition_catalog_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(partition_catalog_tests PRIVATE usb_vault_core)
usb_vault_configure_target(partition_catalog_tests)
add_test(NAME partition_catalog_tests COMMAND partition_catalog_tests)
# --scan is an explicit read-only native smoke test; not part of default CTest.

# Explicitly built and invoked only; no existing device input, never default CTest.
add_executable(partition_device_integration_tests EXCLUDE_FROM_ALL partition_device_integration_tests.cpp)
target_include_directories(partition_device_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(partition_device_integration_tests PRIVATE usb_vault_core VirtDisk Ole32 Bcrypt)
usb_vault_configure_target(partition_device_integration_tests)
if(USB_VAULT_FORMAT_DLL)
  add_custom_command(TARGET partition_device_integration_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${USB_VAULT_FORMAT_DLL}" "$<TARGET_FILE_DIR:partition_device_integration_tests>/VeraCryptFormat.dll")
endif()

add_executable(drive_letter_pool_tests drive_letter_pool_tests.cpp)
target_include_directories(drive_letter_pool_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(drive_letter_pool_tests PRIVATE usb_vault_core)
usb_vault_configure_target(drive_letter_pool_tests)
add_test(NAME drive_letter_pool_tests COMMAND drive_letter_pool_tests)

add_executable(partition_inplace_plan_tests partition_inplace_plan_tests.cpp)
target_include_directories(partition_inplace_plan_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(partition_inplace_plan_tests PRIVATE usb_vault_core)
usb_vault_configure_target(partition_inplace_plan_tests)
add_test(NAME partition_inplace_plan_tests COMMAND partition_inplace_plan_tests)

add_executable(partition_inplace_runner_tests partition_inplace_runner_tests.cpp)
target_include_directories(partition_inplace_runner_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(partition_inplace_runner_tests PRIVATE usb_vault_core)
usb_vault_configure_target(partition_inplace_runner_tests)
add_test(NAME partition_inplace_runner_tests COMMAND partition_inplace_runner_tests)

add_executable(elevation_session_integration_tests EXCLUDE_FROM_ALL elevation_session_integration_tests.cpp)
target_include_directories(elevation_session_integration_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(elevation_session_integration_tests PRIVATE usb_vault_core)
usb_vault_configure_target(elevation_session_integration_tests)

add_executable(mount_state_monitor_tests mount_state_monitor_tests.cpp)
target_link_libraries(mount_state_monitor_tests PRIVATE usb_vault_core)
usb_vault_configure_target(mount_state_monitor_tests)
add_test(NAME mount_state_monitor_tests COMMAND mount_state_monitor_tests)

add_executable(vikey_card_backup_tests vikey_card_backup_tests.cpp)
target_include_directories(vikey_card_backup_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(vikey_card_backup_tests PRIVATE usb_vault_vikey_helper_card_record_backend)
usb_vault_configure_target(vikey_card_backup_tests)
add_test(NAME vikey_card_backup_tests COMMAND vikey_card_backup_tests)
