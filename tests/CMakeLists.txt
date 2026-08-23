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

add_executable(fixed_blob_card_store_tests
  fixed_blob_card_store_tests.cpp
)

target_link_libraries(fixed_blob_card_store_tests
  PRIVATE
    usb_vault_core
)

usb_vault_configure_target(fixed_blob_card_store_tests)

add_test(NAME fixed_blob_card_store_tests COMMAND fixed_blob_card_store_tests)

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
            "${CMAKE_CURRENT_SOURCE_DIR}/webview_typed_array_integration.html"
            "$<TARGET_FILE_DIR:webview_typed_array_integration_tests>/webview_typed_array_integration.html"
    VERBATIM
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
