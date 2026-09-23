# Hash built helpers and source UI only at build time. No mutable manifest is
# trusted by the running application. copy_if_different avoids needless links.
set(text "#pragma once\nnamespace p2_deployment {\n")
foreach(name IN ITEMS helper creator html js)
  file(SHA256 "${${name}}" digest)
  string(APPEND text "inline constexpr wchar_t ${name}[] = L\"${digest}\";\n")
endforeach()
if(DEFINED app)
  file(SHA256 "${app}" digest)
  string(APPEND text "inline constexpr wchar_t app[] = L\"${digest}\";\n")
endif()
string(APPEND text "inline constexpr wchar_t vendor[] = L\"735A2347F66F10C649C9B38ED0232B839812277C8E178F0C5084C676CF1DE48E\";\n")
string(APPEND text "inline constexpr wchar_t format[] = L\"D911EEE4B02177E3BE82970EAC2B8EA6886DE07D23801D6316E5D9E89C2B4CB0\";\n}\n")
file(WRITE "${output}.tmp" "${text}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${output}.tmp" "${output}"
                COMMAND_ERROR_IS_FATAL ANY)
file(REMOVE "${output}.tmp")
