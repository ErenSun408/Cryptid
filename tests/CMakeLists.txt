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
