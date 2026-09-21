#include "volume_creation/veracrypt_format_abi.h"

#include <Windows.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace abi = usb_vault::volume_creation::detail::format_abi;
namespace {
void require(bool ok, const char* why) {
  if (!ok) throw std::runtime_error(why);
}
struct Backend {
  HMODULE module{};
  abi::Shutdown shutdown{};
  ~Backend() {
    if (shutdown) shutdown();
    if (module) ::FreeLibrary(module);
  }
  template<class T> T symbol(const char* name) {
    const auto result = ::GetProcAddress(module, name);
    require(result != nullptr, "missing geometry test export");
    return reinterpret_cast<T>(result);
  }
};
using Header = std::array<unsigned char, abi::kGeometryHeaderBytes>;
Header read_header(std::ifstream& file, std::streamoff offset) {
  Header result{};
  file.seekg(offset);
  file.read(reinterpret_cast<char*>(result.data()), result.size());
  require(file.good(), "cannot read fixture header");
  return result;
}
}

// Explicit, read-only probe for the disposable 128/64 MiB fixture produced by
// decoy_maintenance_integration_tests. Never accept real-user passwords here.
int wmain(int argc, wchar_t** argv) {
  try {
    require(argc == 3, "requires candidate DLL and disposable fixture paths");
    const auto dll = std::filesystem::path(argv[1]);
    const auto fixture = std::filesystem::path(argv[2]);
    require(dll.is_absolute() && fixture.is_absolute(), "absolute paths required");
    require(std::filesystem::file_size(fixture) == 128ULL * 1024 * 1024,
            "expected disposable 128 MiB fixture");
    std::ifstream file(fixture, std::ios::binary);
    const Header outer = read_header(file, 0);
    const Header hidden = read_header(file, 65536);
    const std::string outer_password = "DisposableOuterP3259";
    const std::string hidden_password = "DisposableHiddenP3259";
    Backend backend;
    backend.module = ::LoadLibraryExW(dll.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(backend.module != nullptr, "cannot load candidate DLL");
    const auto query = backend.symbol<abi::ReadHiddenGeometry>("VeraCryptReadHiddenGeometry");
    std::array<std::uint64_t, 4> geometry{};
    auto call = [&](const Header& a, const Header& b, const std::string& ap,
                    const std::string& bp, std::uint64_t host = 128ULL * 1024 * 1024,
                    std::uint32_t header_bytes = abi::kGeometryHeaderBytes) {
      geometry.fill(UINT64_MAX);
      return query(a.data(), header_bytes, b.data(), abi::kGeometryHeaderBytes,
          reinterpret_cast<const unsigned char*>(ap.data()), static_cast<std::uint32_t>(ap.size()),
          reinterpret_cast<const unsigned char*>(bp.data()), static_cast<std::uint32_t>(bp.size()),
          host, geometry.data(), abi::kGeometryOutputBytes);
    };
    require(call(outer, hidden, outer_password, hidden_password) != abi::kSuccess,
            "uninitialized query accepted");
    require(geometry == std::array<std::uint64_t,4>{}, "uninitialized output leaked");
    require(backend.symbol<abi::Initialize>("VeraCryptFormat_Initialize")() == abi::kSuccess,
            "candidate DLL initialization failed");
    backend.shutdown = backend.symbol<abi::Shutdown>("VeraCryptFormat_Shutdown");
    require(call(outer, hidden, outer_password, hidden_password) == abi::kSuccess,
            "valid primary headers rejected");
    // GetVolumeDataAreaSize reserves 128 KiB for hidden fixtures >= 2 MiB.
    require(geometry == std::array<std::uint64_t,4>{131072, 133955584, 66977792, 66977792},
            "geometry differs from actual 128/64 MiB creation layout");
    std::cout << "PASS actual protected geometry differs from requested hidden size\n";
    const auto good = geometry;
    const auto reject = [&](const Header& a, const Header& b, const std::string& ap,
                            const std::string& bp, std::uint64_t host = 128ULL * 1024 * 1024,
                            std::uint32_t bytes = abi::kGeometryHeaderBytes) {
      const int result = call(a, b, ap, bp, host, bytes);
      require(result != abi::kSuccess, "invalid geometry input accepted");
      require(geometry == std::array<std::uint64_t,4>{}, "failed query returned geometry");
      return result;
    };
    const int wrong_outer = reject(outer, hidden, "wrong", hidden_password);
    const int wrong_hidden = reject(outer, hidden, outer_password, "wrong");
    Header corrupted = hidden;
    corrupted[96] ^= 0x80;
    require(reject(outer, corrupted, outer_password, hidden_password) == wrong_outer &&
            wrong_outer == wrong_hidden, "header failures expose distinct reasons");
    reject(hidden, outer, hidden_password, outer_password);
    reject(outer, hidden, outer_password + std::string("\0suffix", 7), hidden_password);
    reject(outer, hidden, outer_password, hidden_password + std::string("\0suffix", 7));
    reject(outer, hidden, outer_password, hidden_password, 127ULL * 1024 * 1024);
    reject(outer, hidden, outer_password, hidden_password, UINT64_MAX);
    reject(outer, hidden, "", hidden_password);
    reject(outer, hidden, std::string(129, 'x'), hidden_password);
    reject(outer, hidden, outer_password, hidden_password, 128ULL * 1024 * 1024, 511);
    geometry.fill(UINT64_MAX);
    require(query(nullptr, 512, hidden.data(), 512,
        reinterpret_cast<const unsigned char*>(outer_password.data()),
        static_cast<std::uint32_t>(outer_password.size()),
        reinterpret_cast<const unsigned char*>(hidden_password.data()),
        static_cast<std::uint32_t>(hidden_password.size()),
        128ULL * 1024 * 1024, geometry.data(), abi::kGeometryOutputBytes) == abi::kInvalidParameter &&
        geometry == std::array<std::uint64_t,4>{}, "null input was not rejected and cleared");
    require(query(nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0, nullptr, 32) ==
        abi::kInvalidParameter, "null output accepted");
    geometry.fill(17);
    require(query(nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0, geometry.data(), 31) ==
        abi::kInvalidParameter && geometry == std::array<std::uint64_t,4>{17,17,17,17},
        "invalid output length accepted or overwritten");
    require(call(outer, hidden, outer_password, hidden_password) == abi::kSuccess && geometry == good,
            "valid query after failures changed geometry");
    require(read_header(file, 0) == outer && read_header(file, 65536) == hidden,
            "fixture headers changed during read-only query");
    std::cout << "PASS wrong passwords, corruption, swapped headers, embedded NUL suffixes, sizes, repeated calls\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
