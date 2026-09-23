#include "volume_creation/partition_inplace_backend.h"
#include <array>
#include <stdexcept>
#include <utility>
namespace usb_vault::volume_creation::detail {
namespace {
void checked(int code) { if (code != 0) throw std::system_error(make_error_code(format_abi::map_error(code))); }
void seek(HANDLE target, std::uint64_t offset) {
  LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
  if (!::SetFilePointerEx(target, position, nullptr, FILE_BEGIN)) throw std::runtime_error("in-place seek failed");
}
}
PartitionInplaceBackend::PartitionInplaceBackend(HMODULE module, HANDLE target, std::uint64_t offset,
    std::uint64_t size, const security::SecureBuffer& password, bool begin,
    std::function<volume::detail::InplaceControl()> control, ProgressCallback progress, std::array<std::uint8_t, 16> partition_id)
    : target_(target), offset_(offset), size_(size), password_(password), begin_(begin),
      control_(std::move(control)), progress_(std::move(progress)), partition_id_(partition_id) {
  if (!module || !target || target == INVALID_HANDLE_VALUE || password.size()!=64 || !control_)
    throw std::invalid_argument("invalid in-place backend arguments");
  open_=reinterpret_cast<format_abi::InplaceOpen>(::GetProcAddress(module,"VeraCryptInplaceOpenBinary"));
  if (partition_id_ != std::array<std::uint8_t, 16>{}) {
    open_gpt_=reinterpret_cast<format_abi::InplaceOpenGpt>(::GetProcAddress(module,"VeraCryptInplaceOpenGptBinary"));
    if (!open_gpt_) throw std::runtime_error("GPT in-place backend unavailable");
  }
  encrypt_=reinterpret_cast<format_abi::InplaceEncrypt>(::GetProcAddress(module,"VeraCryptInplaceEncrypt"));
  save_=reinterpret_cast<format_abi::InplaceSave>(::GetProcAddress(module,"VeraCryptInplaceSave"));
  finalize_=reinterpret_cast<format_abi::InplaceFinalize>(::GetProcAddress(module,"VeraCryptInplaceFinalize"));
  close_=reinterpret_cast<format_abi::InplaceClose>(::GetProcAddress(module,"VeraCryptInplaceClose"));
  if (!open_ || !encrypt_ || !save_ || !finalize_ || !close_) throw std::runtime_error("in-place backend unavailable");
}
volume::detail::InplaceControl PartitionInplaceBackend::control() {
  if (opened_ && progress_) progress_({CreationStage::formatting,static_cast<std::uint32_t>(converted_*100/(size_-262144))});
  return control_();
}
volume::detail::InplaceRange PartitionInplaceBackend::open_authenticated() {
  std::array<std::uint64_t,3> range{};
  const auto allowed = [](void* context) -> BOOL {
    try { return static_cast<PartitionInplaceBackend*>(context)->control_() != volume::detail::InplaceControl::revoked; }
    catch (...) { return FALSE; }
  };
  const auto* secret = reinterpret_cast<const unsigned char*>(password_.data());
  checked(open_gpt_ ? open_gpt_(target_,offset_,size_,secret,64,begin_ ? TRUE : FALSE,
      range.data(),sizeof(range),allowed,this,partition_id_.data(),16)
      : open_(target_,offset_,size_,secret,64,begin_ ? TRUE : FALSE,range.data(),sizeof(range),allowed,this));
  opened_=true; converted_=range[2];
  if (range[0]!=size_) throw std::runtime_error("in-place backend size mismatch");
  return {range[0],range[1],range[2]};
}
std::size_t PartitionInplaceBackend::read(std::uint64_t offset,std::span<std::byte> buffer) {
  if (!opened_ || buffer.size()>65536 || offset>size_-262144 || buffer.size()>size_-262144-offset)
    throw std::invalid_argument("in-place read bounds");
  seek(target_,offset); DWORD done{};
  if (!::ReadFile(target_,buffer.data(),static_cast<DWORD>(buffer.size()),&done,nullptr)) throw std::runtime_error("in-place read failed");
  return done;
}
void PartitionInplaceBackend::encrypt(std::uint64_t unit,std::span<std::byte> buffer) {
  checked(encrypt_(unit,reinterpret_cast<unsigned char*>(buffer.data()),static_cast<std::uint32_t>(buffer.size())));
}
std::size_t PartitionInplaceBackend::write(std::uint64_t offset,std::span<const std::byte> buffer) {
  if (!opened_ || buffer.size()>65536 || offset<131072 || offset>size_-131072 || buffer.size()>size_-131072-offset)
    throw std::invalid_argument("in-place write bounds");
  seek(target_,offset); DWORD done{};
  if (!::WriteFile(target_,buffer.data(),static_cast<DWORD>(buffer.size()),&done,nullptr)) throw std::runtime_error("in-place write failed");
  if (done==buffer.size()) converted_+=done;
  return done;
}
void PartitionInplaceBackend::save_progress(const volume::detail::InplaceRange& range) { checked(save_(range.start,range.length)); }
void PartitionInplaceBackend::flush() { if (!opened_ || !::FlushFileBuffers(target_)) throw std::runtime_error("in-place flush failed"); }
void PartitionInplaceBackend::finalize() { checked(finalize_()); }
void PartitionInplaceBackend::close() noexcept { if (opened_) { close_(); opened_=false; } }
}
