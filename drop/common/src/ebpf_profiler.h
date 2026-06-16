#pragma once

#include "profiler.h"

#include <string>
#include <vector>

namespace drop {

// eBPF off-CPU profiler.
// Uses EbpfLoader to trace sched_switch events for the specified duration,
// producing a folded-stack file suitable for flame graph generation.
class EbpfProfiler : public IProfiler {
public:
  bool record(const drop::hotmethod::TaskDesc& task,
              const std::string& output_dir,
              std::string* error_msg) override;

  std::vector<std::string> collect_result() const override;

  uint32_t profiler_type() const override;

private:
  std::vector<std::string> collected_files_;
};

}  // namespace drop
