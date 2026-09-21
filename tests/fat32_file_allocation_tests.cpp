#include "volume/fat32_file_allocation_detail.h"
#include <array>
#include <iostream>
using namespace usb_vault::volume;
using namespace usb_vault::volume::detail;
namespace {
void check(bool x){if(!x)throw std::runtime_error("file allocation assertion");}
template<class E=std::invalid_argument,class F> void rejects(F f){try{f();}catch(const E&){return;}throw std::runtime_error("invalid extent accepted");}
void feed(Fat32ExtentCollector& c,std::int64_t start,std::initializer_list<Fat32Extent> page){c.consume(start,{page.begin(),page.size()});}
void valid_mapping(){
  Fat32ExtentCollector c(10,100);
  feed(c,0,{{2,50},{5,10}});check(c.next()==5&&!c.complete());
  rejects([&]{static_cast<void>(c.finish());});
  feed(c,5,{{8,13},{10,70}});
  check(c.finish()==std::vector<Fat32ClusterRun>{{50,2},{10,6},{70,2}});
  rejects([&]{static_cast<void>(c.finish());});
  Fat32ExtentCollector empty(0,100);check(empty.finish().empty());
}
void malformed(){
  rejects([]{Fat32ExtentCollector c(101,100);});
  rejects([]{Fat32ExtentCollector c(0,0);});
  for(const auto e:std::array{Fat32Extent{0,1},Fat32Extent{-1,1},Fat32Extent{1,-1},Fat32Extent{11,1},Fat32Extent{2,99},Fat32Extent{1,100}}){
    Fat32ExtentCollector c(10,100);rejects([&]{feed(c,0,{e});});
  }
  Fat32ExtentCollector gap(10,100);rejects([&]{feed(gap,1,{{10,10}});});rejects([&]{feed(gap,-1,{{10,10}});});
  rejects([&]{feed(gap,0,{});});feed(gap,0,{{5,10}});
  rejects([&]{feed(gap,0,{{10,20}});});rejects([&]{feed(gap,6,{{10,20}});});
  Fat32ExtentCollector overlap(10,100);feed(overlap,0,{{5,10},{10,12}});
  rejects([&]{static_cast<void>(overlap.finish());});
  Fat32ExtentCollector duplicate(10,100);feed(duplicate,0,{{5,10},{10,10}});
  rejects([&]{static_cast<void>(duplicate.finish());});
  Fat32ExtentCollector repeated(10,100);rejects([&]{feed(repeated,0,{{5,1},{5,10}});});
  Fat32ExtentCollector fragmented(kMaxAllocationPlanRuns+1,2*kMaxAllocationPlanRuns+2);
  for(std::size_t n=0;n<kMaxAllocationPlanRuns;++n)feed(fragmented,static_cast<std::int64_t>(n),{{static_cast<std::int64_t>(n+1),static_cast<std::int64_t>(2*n)}});
  rejects<std::length_error>([&]{feed(fragmented,kMaxAllocationPlanRuns,{{kMaxAllocationPlanRuns+1,2*kMaxAllocationPlanRuns}});});
}
void permutation(){
  for(unsigned shift=0;shift<97;++shift){
    Fat32ExtentCollector c(97,1000);std::vector<std::uint64_t> expected;
    for(std::int64_t v=0;v<97;++v){const auto lcn=(v*31+shift)%97;feed(c,v,{{v+1,lcn}});expected.push_back(static_cast<std::uint64_t>(lcn));}
    std::vector<std::uint64_t> actual;for(const auto& r:c.finish())for(auto n=r.first_cluster;n<r.first_cluster+r.cluster_count;++n)actual.push_back(n);
    check(actual==expected);
  }
}
}
int main(){try{valid_mapping();malformed();permutation();std::cout<<"fat32_file_allocation_tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
