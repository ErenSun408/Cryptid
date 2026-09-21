#include "usb_vault/application/card_reset_confirmation.h"
#include <stdexcept>
#include <iostream>
using usb_vault::application::CardResetConfirmation;
void require(bool ok) { if (!ok) throw std::runtime_error("reset confirmation invariant failed"); }
int main() {
  try {
    CardResetConfirmation state;
    require(!state.consume(0));
    const auto first = state.prepare();
    require(state.matches(first));
    state.cancel();
    require(!state.consume(first));
    const auto second = state.prepare();
    require(second != first && !state.matches(first));
    require(state.consume(second) && !state.consume(second));
    const auto third = state.prepare();
    require(!state.consume(first) && !state.matches(third));
    const auto fourth = state.prepare();
    const auto fifth = state.prepare();
    require(!state.matches(fourth) && state.consume(fifth));
    std::cout << "card_reset_confirmation_tests passed\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what(); return 1; }
}
