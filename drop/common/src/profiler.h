#pragma once

#include <string>
#include <vector>
#include "hotmethod.pb.h"

namespace drop {

// Abstract profiler interface.
// Each profiler type (perf, eBPF, async-profiler, etc.) implements this.
class IProfiler {
public:
  virtual ~IProfiler() = default;

  // Start collection. Blocks until collection finishes or fails.
  // On success, the output file is written to the task's output directory.
  // On failure, returns false and sets *error_msg.
  virtual bool record(const drop::hotmethod::TaskDesc& task,
                      const std::string& output_dir,
                      std::string* error_msg) = 0;

  // Returns the relative path(s) of collected file(s) within output_dir.
  // Called after record() succeeds.
  virtual std::vector<std::string> collect_result() const = 0;

  // Returns the profiler type integer matching proto ProfilerType.
  virtual uint32_t profiler_type() const = 0;
};

}  // namespace drop
