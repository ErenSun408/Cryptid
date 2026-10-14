#include "card/vikey/vikey_card_backup.h"
#include "usb_vault/card/card_record_v1.h"
#include "usb_vault/ui/card_backup_exchange.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
using namespace usb_vault::card;
using namespace usb_vault::card::vikey;
using usb_vault::security::SecureBuffer;
namespace {
void check(bool value) { if (!value) throw std::runtime_error("backup assertion failed"); }
SecureBuffer copy(const SecureBuffer& value) {
  SecureBuffer result(value.size()); std::ranges::copy(value.bytes(), result.bytes().begin()); return result;
}
void u32(SecureBuffer& b, std::size_t offset, std::uint32_t value) {
  for (unsigned int i=0; i<4; ++i) b.bytes()[offset+i]=std::byte((value>>(i*8))&255);
}
SecureBuffer framed(std::size_t length) {
  SecureBuffer b(length); std::ranges::fill(b.bytes(), std::byte{0xa5});
  constexpr std::array magic{std::byte{'U'},std::byte{'V'},std::byte{'C'},std::byte{'R'},std::byte{'D'},std::byte{'2'},std::byte{},std::byte{}};
  std::ranges::copy(magic,b.bytes().begin());
  u32(b,8,1); u32(b,12,216); u32(b,48,512*1024); u32(b,52,2); u32(b,56,1); u32(b,60,0x13); return b;
}
struct Backend : IVikeyCardBackupBackend {
  VikeyCardMediaProbe state{.hardware_id=1,.media_state=VikeyCardMediaState::product_evidence,.logical_state=CardRecordState::initialized_v1};
  SecureBuffer data=framed(292);
  int reads=0,writes=0; bool mutate=false,fail_write=false,bad_readback=false;
  VikeyCardMediaProbe probe() override { return state; }
  SecureBuffer snapshot(std::uint32_t id) override {
    check(id==state.hardware_id); ++reads; auto b=copy(data);
    if ((mutate && reads==2) || (bad_readback && writes)) b.bytes().back()=std::byte{0}; return b;
  }
  void write(std::uint32_t id,const SecureBuffer& b) override {
    check(id==2); ++writes; if(fail_write) throw std::runtime_error("injected write"); data=copy(b);
  }
  void target() { state={.hardware_id=2,.media_state=VikeyCardMediaState::blank}; }
};
template<class F> void rejects(F f) { bool rejected=false; try { f(); } catch (...) { rejected=true; } check(rejected); }
void tests() {
  using T=usb_vault::application::CardPresenceTransition;
  for (auto outcome : {T::card_unavailable,T::active_session_unmounted,T::active_session_force_unmounted}) {
    usb_vault::ui::CardBackupExchange exchange;
    check(!exchange.allow_removal(T::no_change));
    check(!exchange.allow_removal(T::active_session_remains));
    check(!exchange.removed()); check(exchange.allow_removal(outcome));
    check(exchange.removed()); check(!exchange.allow_removal(outcome));
    check(exchange.allow_removal(outcome,true));
    check(exchange.allow_removal(T::card_unavailable,true));
    check(!exchange.allow_removal(T::active_session_remains,true));
    check(!exchange.allow_removal(T::no_change,true));
    check(!exchange.allow_removal(outcome));
  }
  {
    usb_vault::ui::CardBackupTargetReadiness readiness;
    const auto time=usb_vault::ui::CardBackupTargetReadiness::Clock::now();
    readiness.observe(true,time); const auto first=readiness.generation();
    check(!readiness.accepts(first,time+std::chrono::milliseconds(599)));
    check(readiness.accepts(first,time+std::chrono::milliseconds(600)));
    check(readiness.observe(false,time+std::chrono::milliseconds(610)));
    check(!readiness.accepts(first,time+std::chrono::seconds(1)));
    readiness.observe(true,time+std::chrono::milliseconds(620));
    check(!readiness.accepts(first,time+std::chrono::seconds(2)));
    check(!readiness.stable(time+std::chrono::milliseconds(1219)));
    check(readiness.accepts(readiness.generation(),time+std::chrono::milliseconds(1220)));
    readiness.invalidate(); check(!readiness.stable(time+std::chrono::seconds(3)));
  }
  {
    usb_vault::ui::CardBackupProbeRetry retry;
    const auto time=usb_vault::ui::CardBackupProbeRetry::Clock::now();
    check(retry.due(time)); retry.failed(time);
    check(!retry.due(time+std::chrono::milliseconds(999)));
    check(retry.due(time+std::chrono::seconds(1)));
    retry.failed(time+std::chrono::seconds(1));
    check(!retry.due(time+std::chrono::milliseconds(2999)));
    check(retry.due(time+std::chrono::seconds(3)));
    retry.failed(time+std::chrono::seconds(3));
    check(!retry.due(time+std::chrono::milliseconds(6999)));
    check(retry.due(time+std::chrono::seconds(7)));
    retry.failed(time+std::chrono::seconds(7));
    check(retry.due(time+std::chrono::seconds(11)));
    retry.reset(); check(retry.due(time));
    check(usb_vault::ui::retryable_card_backup_probe(make_error_code(CardError::device_not_found)));
    check(usb_vault::ui::retryable_card_backup_probe(make_error_code(CardError::communication_failed)));
    check(usb_vault::ui::retryable_card_backup_probe(make_error_code(CardError::operation_timed_out)));
    for (const auto error : {CardError::authentication_failed, CardError::authentication_locked,
                            CardError::api_key_invalid, CardError::multiple_devices, CardError::permission_denied})
      check(!usb_vault::ui::retryable_card_backup_probe(make_error_code(error)));
    // A timeout must not be reported as a source, blank or credential problem.
    const std::wstring_view timed_out=usb_vault::ui::card_backup_failure_text(make_error_code(CardError::operation_timed_out));
    const std::wstring_view generic=usb_vault::ui::card_backup_failure_text(make_error_code(CardError::invalid_operation));
    check(timed_out.find(L"超时")!=std::wstring_view::npos && timed_out.find(L"供应方凭据")==std::wstring_view::npos);
    check(generic.find(L"超时")==std::wstring_view::npos);
    check(std::wstring_view{usb_vault::ui::card_backup_failure_text({})}==generic);
  }
  // Real encrypted envelopes must unlock to the original root after copying.
  {
    const auto password=[] { constexpr char text[]="Spare-card-pass-2026"; SecureBuffer b(sizeof(text)-1); std::ranges::copy(std::as_bytes(std::span(text,sizeof(text)-1)),b.bytes().begin()); return b; };
    auto initialized=initialize_card_record_and_root_v1(password());
    Backend b; b.data=std::move(initialized.record); VikeyCardBackup flow(b);
    flow.prepare(); b.target(); flow.complete();
    const auto reopened=unlock_card_record_v1(b.data.bytes(),password());
    check(std::ranges::equal(reopened.bytes(),initialized.root_secret.bytes()));
  }
  const auto now=VikeyCardBackup::Clock::now();
  {
    Backend b; VikeyCardBackup f(b); f.prepare(now);
    check(f.is_source(b.probe()) && !f.target_eligible(b.probe(),now));
    b.target(); check(f.target_eligible(b.probe(),now) && b.writes==0 && f.pending(now));
    check(!f.is_source(b.probe()));
    b.state.logical_state=CardRecordState::initialized_v1;
    check(!f.target_eligible(b.probe(),now));
    b.state.media_state=VikeyCardMediaState::product_evidence;
    check(!f.target_eligible(b.probe(),now));
    b.state.logical_state=CardRecordState::provisioned_uninitialized;
    check(f.target_eligible(b.probe(),now));
    for(auto media : {VikeyCardMediaState::foreign_data,VikeyCardMediaState::provisioning_interrupted,VikeyCardMediaState::conflicting_evidence}) {
      b.state.media_state=media; check(!f.target_eligible(b.probe(),now));
    }
    check(b.writes==0 && f.pending(now));
    b.target(); check(!f.target_eligible(b.probe(),now+std::chrono::minutes(3)) && !f.pending(now));
  }
  { Backend b; VikeyCardBackup f(b); f.prepare(now); b.target();
    check(f.target_eligible(b.probe(),now));
    f.complete(now,2); check(b.writes==1); }
  { Backend b; VikeyCardBackup f(b); f.prepare(now); b.target();
    check(f.target_eligible(b.probe(),now)); b.state.hardware_id=3;
    rejects([&]{f.complete(now,2);}); check(b.writes==0 && !f.pending(now)); }
  { Backend b; VikeyCardBackup f(b); f.prepare(now); b.target();
    check(f.target_eligible(b.probe(),now)); b.state.media_state=VikeyCardMediaState::foreign_data;
    rejects([&]{f.complete(now,2);}); check(b.writes==0 && !f.pending(now)); }
  for (auto length : {216u,292u}) {
    Backend b; b.data=framed(length); auto expected=copy(b.data); VikeyCardBackup flow(b);
    flow.prepare(now); check(flow.pending(now) && b.writes==0 && b.reads==2);
    b.target(); flow.complete(now); check(!flow.pending(now) && b.writes==1 && std::ranges::equal(expected.bytes(),b.data.bytes()));
  }
  { Backend b; b.mutate=true; VikeyCardBackup f(b); rejects([&]{f.prepare(now);}); check(!f.pending(now)&&b.writes==0); }
  { Backend b; VikeyCardBackup f(b); f.prepare(now); rejects([&]{f.complete(now);}); check(!f.pending(now)&&b.writes==0); }
  for(auto media : {VikeyCardMediaState::foreign_data,VikeyCardMediaState::provisioning_interrupted,VikeyCardMediaState::conflicting_evidence}) {
    Backend b; VikeyCardBackup f(b); f.prepare(now); b.target(); b.state.media_state=media;
    rejects([&]{f.complete(now);}); check(!f.pending(now)&&b.writes==0);
  }
  { Backend b; VikeyCardBackup f(b); f.prepare(now); b.state.hardware_id=2; rejects([&]{f.complete(now);}); check(b.writes==0); }
  { Backend b; VikeyCardBackup f(b); f.prepare(now);
    check(f.remaining_seconds(now)==180);
    check(f.remaining_seconds(now+std::chrono::seconds(61))==119);
    check(f.remaining_seconds(now+std::chrono::seconds(179))==1);
    b.target(); rejects([&]{f.complete(now+std::chrono::minutes(3));});
    check(b.writes==0&&!f.pending(now)&&f.remaining_seconds(now)==0); }
  { Backend b; VikeyCardBackup f(b); f.prepare(now); f.cancel(); b.target(); rejects([&]{f.complete(now);}); check(b.writes==0); }
  for(bool corrupt : {false,true}) {
    Backend b; VikeyCardBackup f(b); f.prepare(now); b.target(); b.fail_write=!corrupt; b.bad_readback=corrupt;
    rejects([&]{f.complete(now);}); check(!f.pending(now)); rejects([&]{f.complete(now);}); check(b.writes==1);
  }
  { Backend b; VikeyCardBackup f(b); f.prepare(now); b.target(); b.state.media_state=VikeyCardMediaState::product_evidence; b.state.logical_state=CardRecordState::provisioned_uninitialized; f.complete(now); check(b.writes==1); }
}
}
int main() { try { tests(); std::cout<<"spare card workflow tests passed\n"; return 0; } catch(const std::exception& e) { std::cerr<<e.what(); return 1; } }
