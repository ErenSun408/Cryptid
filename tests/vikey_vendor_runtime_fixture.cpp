#include <Windows.h>

#include <algorithm>
#include <iterator>

extern "C" __declspec(dllexport) DWORD __stdcall
VikeyGetLibraryVersion(char* const version) {
  if (version == nullptr) return ERROR_INVALID_PARAMETER;
  constexpr char kVersion[] = "fixture-1";
  std::copy(std::begin(kVersion), std::end(kVersion), version);
  return ERROR_SUCCESS;
}
