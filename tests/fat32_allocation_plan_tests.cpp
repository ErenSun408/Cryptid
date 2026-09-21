#include "volume/fat32_allocation_plan_detail.h"
#include <algorithm>
#include <iostream>
#include <limits>
using namespace usb_vault::volume;
using namespace usb_vault::volume::detail;
namespace {
void check(bool valid) { if(!valid) throw std::runtime_error("allocation plan assertion failed"); }
template<class E, class F> void rejects(F action) {
  try {action();} catch(const E&) {return;}
  throw std::runtime_error("invalid allocation plan accepted");
}
void bit(std::vector<std::byte>& data, std::uint64_t cluster, bool used) {
  const auto mask=static_cast<unsigned char>(1U<<(cluster%8));
  auto value=std::to_integer<unsigned char>(data[static_cast<std::size_t>(cluster/8)]);
  data[static_cast<std::size_t>(cluster/8)]=static_cast<std::byte>(used?value|mask:value&~mask);
}
Fat32AllocationPlan plan(Fat32Geometry g, const std::vector<std::byte>& bits, std::uint64_t size) {
  Fat32BitmapCounter counter(g);Fat32FirstFitPlanner planner(g,size);
  auto offer=[&](std::uint64_t cluster){planner.free_cluster(cluster);};
  // Deliberately overlap the first page to exercise Windows round-down behavior.
  counter.consume(0,static_cast<std::int64_t>(g.cluster_count),std::span(bits).first(1),offer);
  counter.consume(0,static_cast<std::int64_t>(g.cluster_count),bits,offer);
  check(counter.complete());return planner.finish();
}
void mark(std::vector<std::byte>& data,const Fat32AllocationPlan& p) {
  for(const auto& run:p.runs)for(auto n=run.first_cluster;n<run.first_cluster+run.cluster_count;++n)bit(data,n,true);
}
void five_holes() {
  Fat32Geometry g{0,1024,65536,45056};std::vector<std::byte> bits(8192,std::byte{0xff});
  const std::vector<Fat32ClusterRun> holes{{100,1024},{2000,2048},{5000,3072},{9000,4096},{14000,5120}};
  for(const auto& r:holes)for(auto n=r.first_cluster;n<r.first_cluster+r.cluster_count;++n)bit(bits,n,false);
  auto p=plan(g,bits,8ULL*1024*1024);
  check(p.runs==std::vector<Fat32ClusterRun>{{100,1024},{2000,2048},{5000,3072},{9000,2048}});mark(bits,p);
  p=plan(g,bits,3ULL*1024*1024);check(p.runs==std::vector<Fat32ClusterRun>{{11048,2048},{14000,1024}});mark(bits,p);
  p=plan(g,bits,4ULL*1024*1024);check(p.runs==std::vector<Fat32ClusterRun>{{15024,4096}});mark(bits,p);
  rejects<std::system_error>([&]{plan(g,bits,1);});check(plan(g,bits,0).runs.empty());
}
void bounds() {
  Fat32Geometry g{0,1024,32,4};std::vector<std::byte> bits(4,std::byte{0});
  auto p=plan(g,bits,1025);check(p.allocation_bytes==2048&&p.file_bytes==1025&&p.runs==std::vector<Fat32ClusterRun>{{0,2}});
  p=plan(g,bits,4096);check(p.runs==std::vector<Fat32ClusterRun>{{0,4}});
  rejects<std::system_error>([&]{plan(g,bits,4097);});
  rejects<std::length_error>([&]{Fat32FirstFitPlanner tooLarge(g,1ULL<<32);});
  rejects<std::length_error>([&]{Fat32FirstFitPlanner overflow(g,UINT64_MAX);});
  g.cluster_size=1000;rejects<std::invalid_argument>([&]{Fat32FirstFitPlanner invalid(g,1);});
  g={0,524288,10000,10000};Fat32FirstFitPlanner maximum(g,UINT32_MAX);
  for(std::uint64_t n=0;n<8192;++n)maximum.free_cluster(n);
  p=maximum.finish();check(p.file_bytes==UINT32_MAX&&p.allocation_bytes==(1ULL<<32)&&p.runs.size()==1);
  rejects<std::invalid_argument>([&]{static_cast<void>(maximum.finish());});
  Fat32FirstFitPlanner duplicate(g,1);duplicate.free_cluster(1);
  rejects<std::invalid_argument>([&]{duplicate.free_cluster(1);});
  rejects<std::invalid_argument>([&]{duplicate.free_cluster(10000);});
  Fat32Geometry split{0,512,2*kMaxAllocationPlanRuns+2,2*kMaxAllocationPlanRuns+2};
  Fat32FirstFitPlanner fragmented(split,(kMaxAllocationPlanRuns+1ULL)*512);
  for(std::uint64_t n=0;n<kMaxAllocationPlanRuns;++n)fragmented.free_cluster(2*n);
  rejects<std::length_error>([&]{fragmented.free_cluster(2*kMaxAllocationPlanRuns);});
}
void randomized_oracle() {
  std::uint32_t seed=0x31a947;
  auto next=[&]{seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
  for(unsigned trial=0;trial<100;++trial) {
    Fat32Geometry g{0,512,2048,1500};std::vector<std::byte> bits(256,std::byte{0xff});
    std::vector<std::uint64_t> expected;
    for(std::uint64_t n=0;n<g.cluster_count;++n)if(next()%3==0){bit(bits,n,false);if(n<g.safe_clusters)expected.push_back(n);}
    const auto count=next()%(expected.size()+1);
    const auto bytes=count?count*512-(next()%512):0;
    auto p=plan(g,bits,bytes);std::vector<std::uint64_t> actual;
    for(const auto& r:p.runs)for(auto n=r.first_cluster;n<r.first_cluster+r.cluster_count;++n)actual.push_back(n);
    expected.resize(count);check(actual==expected&&p.allocation_bytes==count*512);
  }
}
}
int main(){try{five_holes();bounds();randomized_oracle();std::cout<<"fat32_allocation_plan_tests passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
