#include "volume/fat32_safe_space_detail.h"
#include <array>
#include <iostream>
#include <vector>
using namespace usb_vault::volume::detail;
void check(bool ok) { if (!ok) throw std::runtime_error("safe-space assertion"); }
template<class F> void rejects(F f) {try {f();}catch(const std::invalid_argument&){return;}throw std::runtime_error("invalid geometry accepted");}
int main() {
  try {
    std::array<std::byte,512> boot{};
    auto set=[&](std::size_t at,std::uint32_t value,unsigned count) {for(unsigned i=0;i<count;++i)boot[at+i]=static_cast<std::byte>((value>>(8*i))&255);};
    set(11,512,2);set(13,1,1);set(14,32,2);set(16,2,1);set(32,71128,4);set(36,548,4);set(44,2,4);set(510,0xaa55,2);
    const std::uint64_t length=71128ULL*512,data=1128ULL*512;
    auto g=parse_fat32_geometry(boot,length,data+10*512+511);
    check(g.cluster_count==70000&&g.safe_clusters==10&&g.cluster_size==512);
    std::vector<std::byte> payload(8750,std::byte{0});payload[0]=std::byte{0x05};
    Fat32BitmapCounter full(g);full.consume(0,70000,payload);check(full.bytes()==8*512);
    // Two pages with an overlapping prefix must neither double count nor skip.
    Fat32BitmapCounter pages(g);pages.consume(0,70000,std::span(payload).first(1));check(pages.next()==8);
    rejects([&]{static_cast<void>(pages.bytes());});pages.consume(0,70000,payload);check(pages.bytes()==full.bytes());
    Fat32BitmapCounter gap(g);rejects([&]{gap.consume(1,69999,payload);});rejects([&]{gap.consume(-1,70000,payload);});
    rejects([&]{gap.consume(0,69999,payload);});rejects([&]{gap.consume(0,70000,{});});
    gap.consume(0,70000,std::span(payload).first(1));rejects([&]{gap.consume(0,70000,std::span(payload).first(1));});
    rejects([&]{parse_fat32_geometry(std::span(boot).first(511),length,data+512);});
    rejects([&]{parse_fat32_geometry(boot,length,data);});rejects([&]{parse_fat32_geometry(boot,length,length+1);});
    rejects([&]{parse_fat32_geometry(boot,length-512,data+512);});
    set(13,3,1);rejects([&]{parse_fat32_geometry(boot,length,data+512);});set(13,1,1);
    set(22,1,2);rejects([&]{parse_fat32_geometry(boot,length,data+512);});set(22,0,2);
    set(44,1,4);rejects([&]{parse_fat32_geometry(boot,length,data+512);});set(44,2,4);
    auto none=parse_fat32_geometry(boot,length,data+511);Fat32BitmapCounter noWhole(none);noWhole.consume(0,70000,payload);check(noWhole.bytes()==0);
    std::vector<std::byte> used(8750,std::byte{0xff});Fat32BitmapCounter allUsed(g);allUsed.consume(0,70000,used);check(allUsed.bytes()==0);
    auto bad=g;bad.safe_clusters=bad.cluster_count+1;rejects([&]{Fat32BitmapCounter invalid(bad);});
    set(11,4096,2);auto largeSector=parse_fat32_geometry(boot,71128ULL*4096,1128ULL*4096+10*4096);check(largeSector.safe_clusters==10&&largeSector.cluster_size==4096);set(11,512,2);
    set(32,1056+4000,4);rejects([&]{parse_fat32_geometry(boot,length,data+512);});set(32,71128,4);
    set(36,1,4);rejects([&]{parse_fat32_geometry(boot,length,data+512);});
    std::cout<<"fat32_safe_space_tests passed\n";return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
