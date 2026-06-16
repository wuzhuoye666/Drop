#include "profiler_factory.h"
#include "perf_profiler.h"
#include "async_profiler_profiler.h"
#include "ebpf_profiler.h"

#include <glog/logging.h>

namespace drop {

std::unique_ptr<IProfiler> CreateProfiler(uint32_t profiler_type) {
  switch (profiler_type) {
    case 0:  // perf
      return std::make_unique<PerfProfiler>();
    case 1:  // async-profiler (Java)
      return std::make_unique<AsyncProfilerProfiler>();
    case 3:  // ebpf
      return std::make_unique<EbpfProfiler>();
    default:
      LOG(ERROR) << "Unknown profiler_type=" << profiler_type;
      return nullptr;
  }
}

}  // namespace drop
