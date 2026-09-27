# Release optimization alone does not mean a public release: test packages
# also use Release. The public package preset explicitly selects this gate.
option(USB_VAULT_PRODUCTION_RELEASE "Enforce public release logging policy" OFF)
option(USB_VAULT_DEBUG_LOG "Record detailed test-phase debug events in the runtime log" ON)
if(USB_VAULT_PRODUCTION_RELEASE AND USB_VAULT_DEBUG_LOG)
  message(FATAL_ERROR
    "USB_VAULT_PRODUCTION_RELEASE requires USB_VAULT_DEBUG_LOG=OFF; "
    "detailed test-phase logs must not ship in a public release.")
endif()
