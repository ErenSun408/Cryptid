#include "usb_vault/security/secure_buffer.h"

int main() {
  usb_vault::security::SecureBuffer source(32);
  const usb_vault::security::SecureBuffer copy(source);
  return copy.empty() ? 1 : 0;
}
