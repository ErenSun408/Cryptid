#include "card/vikey/vikey_card_record_io.h"

#include "card/card_record_journal_v1.h"
#include "card/vikey/vikey_session.h"

namespace usb_vault::card::vikey {

class VikeyCardRecordStorage final : public ICardRecordJournalStorage {
 public:
  explicit VikeyCardRecordStorage(VikeySession& session) noexcept
      : session_(session) {}

  [[nodiscard]] std::size_t capacity() override {
    return session_.user_data_capacity();
  }

  [[nodiscard]] security::SecureBuffer read(const std::size_t offset,
                                            const std::size_t length) override {
    return session_.read_user_data(offset, length);
  }

  void write(const std::size_t offset,
             const std::span<const std::byte> bytes) override {
    session_.write_user_data(offset, bytes);
  }

 private:
  VikeySession& session_;
};

security::SecureBuffer read_vikey_card_record(VikeySession& session) {
  VikeyCardRecordStorage storage(session);
  return read_card_record_journal_v1(storage);
}

void write_vikey_card_record(VikeySession& session,
                             const security::SecureBuffer& record) {
  VikeyCardRecordStorage storage(session);
  commit_card_record_journal_v1(storage, record.bytes());
}

}  // namespace usb_vault::card::vikey
