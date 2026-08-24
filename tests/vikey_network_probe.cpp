#include "vikey_network_probe.h"

#include <WinSock2.h>
#if defined(_MSC_VER)
#pragma warning(push)
// MSVC /analyze reports C6101 inside the Windows SDK's inline
// GetIpPathEntry overload. Keep the suppression scoped to that SDK header.
#pragma warning(disable : 6101)
#endif
#include <WS2tcpip.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <utility>

namespace usb_vault::card::vikey::test {
namespace {

enum class AttemptResult {
  denied,
  unavailable,
  not_denied,
};

struct Attempt final {
  AttemptResult result;
  int error;
};

class Winsock final {
 public:
  Winsock() noexcept { result_ = ::WSAStartup(MAKEWORD(2, 2), &data_); }
  ~Winsock() noexcept {
    if (result_ == 0) static_cast<void>(::WSACleanup());
  }

  Winsock(const Winsock&) = delete;
  Winsock& operator=(const Winsock&) = delete;

  [[nodiscard]] int result() const noexcept { return result_; }

 private:
  WSADATA data_{};
  int result_{};
};

class Socket final {
 public:
  explicit Socket(const SOCKET value = INVALID_SOCKET) noexcept
      : value_(value) {}
  ~Socket() noexcept {
    if (value_ != INVALID_SOCKET) static_cast<void>(::closesocket(value_));
  }

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept
      : value_(std::exchange(other.value_, INVALID_SOCKET)) {}
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      if (value_ != INVALID_SOCKET) {
        static_cast<void>(::closesocket(value_));
      }
      value_ = std::exchange(other.value_, INVALID_SOCKET);
    }
    return *this;
  }

  [[nodiscard]] SOCKET get() const noexcept { return value_; }

 private:
  SOCKET value_;
};

[[nodiscard]] bool unsupported_family_error(const int error) noexcept {
  return error == WSAEAFNOSUPPORT || error == WSAEPROTONOSUPPORT ||
         error == WSAEPFNOSUPPORT;
}

[[nodiscard]] bool denied_error(const int error) noexcept {
  // Windows normally reports WSAEACCES for a zero-capability AppContainer.
  // Accept an immediately invalidated fresh socket as an equally unusable
  // path so the probe remains robust across Winsock providers.
  return error == WSAEACCES || error == WSAENOTSOCK;
}

[[nodiscard]] Attempt open_socket(const int family, const int type,
                                  const int protocol, Socket& output) {
  Socket candidate(::socket(family, type, protocol));
  if (candidate.get() == INVALID_SOCKET) {
    const int error = ::WSAGetLastError();
    if (denied_error(error)) return {AttemptResult::denied, error};
    if (unsupported_family_error(error)) {
      return {AttemptResult::unavailable, error};
    }
    return {AttemptResult::not_denied, error};
  }
  output = std::move(candidate);
  return {AttemptResult::not_denied, 0};
}

[[nodiscard]] Attempt tcp_attempt(const sockaddr* const address,
                                  const int address_bytes) {
  Socket socket;
  const Attempt opened =
      open_socket(address->sa_family, SOCK_STREAM, IPPROTO_TCP, socket);
  if (opened.result != AttemptResult::not_denied) return opened;

  u_long nonblocking = 1;
  if (::ioctlsocket(socket.get(), FIONBIO, &nonblocking) == SOCKET_ERROR) {
    const int error = ::WSAGetLastError();
    if (denied_error(error)) return {AttemptResult::denied, error};
    return {AttemptResult::not_denied, error};
  }
  if (::connect(socket.get(), address, address_bytes) == SOCKET_ERROR) {
    const int error = ::WSAGetLastError();
    if (denied_error(error)) return {AttemptResult::denied, error};
    return {AttemptResult::not_denied, error};
  }
  return {AttemptResult::not_denied, 0};
}

[[nodiscard]] Attempt udp_attempt(const sockaddr* const address,
                                  const int address_bytes) {
  Socket socket;
  const Attempt opened =
      open_socket(address->sa_family, SOCK_DGRAM, IPPROTO_UDP, socket);
  if (opened.result != AttemptResult::not_denied) return opened;

  constexpr char kProbeByte = '\0';
  if (::sendto(socket.get(), &kProbeByte, 1, 0, address, address_bytes) ==
      SOCKET_ERROR) {
    const int error = ::WSAGetLastError();
    if (denied_error(error)) return {AttemptResult::denied, error};
    return {AttemptResult::not_denied, error};
  }
  return {AttemptResult::not_denied, 0};
}

}  // namespace

std::uint32_t network_probe_diagnostic() {
  const Winsock winsock;
  if (winsock.result() != 0) {
    return 0x20000000U |
           (static_cast<std::uint32_t>(winsock.result()) & 0xffffU);
  }

  sockaddr_in ipv4_loopback{};
  ipv4_loopback.sin_family = AF_INET;
  ipv4_loopback.sin_port = ::htons(9);
  ipv4_loopback.sin_addr.s_addr = ::htonl(0x7f000001UL);

  sockaddr_in ipv4_documentation{};
  ipv4_documentation.sin_family = AF_INET;
  ipv4_documentation.sin_port = ::htons(9);
  ipv4_documentation.sin_addr.s_addr = ::htonl(0xc0000201UL);

  sockaddr_in6 ipv6_loopback{};
  ipv6_loopback.sin6_family = AF_INET6;
  ipv6_loopback.sin6_port = ::htons(9);
  ipv6_loopback.sin6_addr = IN6ADDR_LOOPBACK_INIT;

  sockaddr_in6 ipv6_documentation{};
  ipv6_documentation.sin6_family = AF_INET6;
  ipv6_documentation.sin6_port = ::htons(9);
  constexpr std::array<unsigned char, 16> kIpv6Documentation{
      0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  std::copy(kIpv6Documentation.begin(), kIpv6Documentation.end(),
            ipv6_documentation.sin6_addr.u.Byte);

  const std::array attempts{
      tcp_attempt(reinterpret_cast<const sockaddr*>(&ipv4_loopback),
                  sizeof(ipv4_loopback)),
      udp_attempt(reinterpret_cast<const sockaddr*>(&ipv4_loopback),
                  sizeof(ipv4_loopback)),
      tcp_attempt(reinterpret_cast<const sockaddr*>(&ipv4_documentation),
                  sizeof(ipv4_documentation)),
      udp_attempt(reinterpret_cast<const sockaddr*>(&ipv4_documentation),
                  sizeof(ipv4_documentation)),
      tcp_attempt(reinterpret_cast<const sockaddr*>(&ipv6_loopback),
                  sizeof(ipv6_loopback)),
      udp_attempt(reinterpret_cast<const sockaddr*>(&ipv6_loopback),
                  sizeof(ipv6_loopback)),
      tcp_attempt(reinterpret_cast<const sockaddr*>(&ipv6_documentation),
                  sizeof(ipv6_documentation)),
      udp_attempt(reinterpret_cast<const sockaddr*>(&ipv6_documentation),
                  sizeof(ipv6_documentation)),
  };

  bool exercised = false;
  for (std::size_t index = 0; index < attempts.size(); ++index) {
    const Attempt& attempt = attempts[index];
    if (attempt.result == AttemptResult::not_denied) {
      return 0x40000000U | (static_cast<std::uint32_t>(index) << 16U) |
             (static_cast<std::uint32_t>(attempt.error) & 0xffffU);
    }
    if (attempt.result == AttemptResult::denied) exercised = true;
  }
  return exercised ? 0U : 0x80000000U;
}

bool all_network_paths_denied() {
  return network_probe_diagnostic() == 0;
}

}  // namespace usb_vault::card::vikey::test
