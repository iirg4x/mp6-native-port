#include "ready_memo.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <random>

struct Info {
  std::array<std::uint64_t, 12> masks{};
  std::uint32_t uniformSize{};
  bool operator==(const Info&) const = default;
};
static unsigned checks;
static void require(bool condition) {
  ++checks;
  if (!condition) { std::fprintf(stderr,"FAIL check %u\n",checks); std::abort(); }
}
static Info make_info(std::uint64_t hash, std::uint32_t alignment) {
  Info result;
  for (unsigned i=0;i<12;++i) result.masks[i]=hash^(std::uint64_t(alignment)<<i);
  result.uniformSize=alignment*4;
  return result;
}
int main() {
  ReadyPipelineMemo<Info> memo;
  Info output=make_info(888,16), sentinel=output;
  require(!memo.get(0,256,0,output) && output==sentinel);
  const auto zero=make_info(0,256);
  memo.remember(0,256,90,zero);
  require(memo.get(0,256,90,output) && output==zero);
  require(memo.get(0,256,91,output) && output==zero);
  require(!memo.get(0,256,89,output));
  require(!memo.get(0,512,90,output));
  const auto collision=make_info(64,256);
  memo.remember(64,256,0,collision);
  require(!memo.get(0,256,90,output));
  require(memo.get(64,256,0,output) && output==collision);
  memo.clear();
  require(!memo.get(64,256,90,output));
  const auto largest=std::numeric_limits<std::uint64_t>::max();
  memo.remember(largest,16,0,zero);
  require(memo.get(largest,16,std::numeric_limits<std::uint32_t>::max(),output));

  // Model the authoritative ready map and persistence. Pending/failed/shutdown
  // results are never remembered; frame rewinds must update authoritative data.
  struct Ready { Info info; std::uint32_t first, alignment; };
  std::map<std::uint64_t,Ready> authoritative;
  std::mt19937_64 rng(0x1237a66);
  std::uint32_t frame=0, alignment=256;
  unsigned hits=0, misses=0, rewinds=0, resets=0, pending=0;
  memo.clear();
  for (unsigned n=0;n<200000;++n) {
    if (n%1709==0) { authoritative.clear(); memo.clear(); ++resets; }
    if (n%271==0) alignment=alignment==256?16:256;
    if (n%307==0) { frame=0; ++rewinds; } else ++frame;
    const auto hash=rng()%100;
    auto it=authoritative.find(hash);
    if (it==authoritative.end() && n%3==0) {
      require(!memo.get(hash,alignment,frame,output));
      ++pending;
      continue;
    }
    if (memo.get(hash,alignment,frame,output)) {
      require(it!=authoritative.end());
      require(frame>=it->second.first);
      require(alignment==it->second.alignment);
      require(output==it->second.info);
      ++hits;
    } else {
      ++misses;
      if (it==authoritative.end()) {
        it=authoritative.emplace(hash,Ready{make_info(hash,alignment),frame,alignment}).first;
      }
      it->second.first=frame<it->second.first?frame:it->second.first;
      if (it->second.alignment!=alignment) {
        it->second.info=make_info(hash,alignment);
        it->second.alignment=alignment;
      }
      output=it->second.info;
      memo.remember(hash,alignment,frame,output);
      Info repeated;
      require(memo.get(hash,alignment,frame,repeated) && repeated==output);
    }
  }
  require(hits && misses && resets && rewinds && pending);
  std::printf("PASS: %u checks; 200000 modeled ready/pending/collision/alignment/rewind/restart requests; %zu-byte bounded memo\n",checks,sizeof(memo));
}
