#include "../src/platform/windows/vikey_configuration/card_failure.h"
#include "card/vikey/vikey_module.h"
#include <stdexcept>
#include <iostream>
int main() {
  using namespace usb_vault::card;
  using namespace usb_vault::card::vikey;
  try {
    auto api = configuration::card_failure(map_status(sdk::kApiKey));
    auto pin = configuration::card_failure(map_status(sdk::kInvalidPassword));
    auto sn = configuration::card_failure(map_status(sdk::kNoDevice));
    if (api.detail.find(L"API Key") == std::wstring::npos ||
        api.detail.find(L"[CARD-11]") == std::wstring::npos ||
        pin.detail.find(L"PIN") == std::wstring::npos ||
        sn.detail.find(L"UserSN") == std::wstring::npos || api.detail == pin.detail)
      throw std::runtime_error("SDK failures lost actionable diagnostic identity");
    for (int value = 1; value <= 18; ++value) {
      auto result = configuration::card_failure(make_error_code(static_cast<CardError>(value)));
      if (result.state != configuration::State::failed || result.detail.find(L"[CARD-") != 0)
        throw std::runtime_error("known failure lost its stable code");
    }
    auto unknown = configuration::card_failure(make_error_code(static_cast<CardError>(999)));
    auto system = configuration::card_failure(std::error_code(5, std::system_category()));
    if (unknown.detail.find(L"999") == std::wstring::npos ||
        system.detail.find(L"SYSTEM-5") == std::wstring::npos)
      throw std::runtime_error("fallback code was lost");
    DiagnosticError start(make_error_code(CardError::backend_unavailable),
        DiagnosticStage::process_start, "SECRET MUST NOT APPEAR", 5, NativeCodeKind::win32);
    auto diagnosed = configuration::card_failure(start);
    if (diagnosed.detail.find(L"D10 Win32=5") == std::wstring::npos ||
        diagnosed.detail.find(L"SECRET") != std::wstring::npos)
      throw std::runtime_error("native diagnostic lost or exception text disclosed");
    DiagnosticError bad(make_error_code(CardError::invalid_configuration),
        DiagnosticStage::credential_key, "secret");
    if (configuration::card_failure(bad).detail.find(L"API Key") == std::wstring::npos)
      throw std::runtime_error("credential field identity lost");
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what(); return 1; }
}
