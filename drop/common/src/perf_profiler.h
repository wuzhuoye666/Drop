#pragma once

#include "profiler.h"

#include <string>
#include <vector>

namespace drop {

// CPU perf profiler using Linux perf tool.
// Executes: perf record -F <hz> --call-graph <callgraph> -e <event> [-p <pid>] -o <path> -- sleep <duration>
// Falls back to system-wide profiling if -p <pid> fails (e.g. PID namespace issues).
class PerfProfiler : public IProfiler {
public:
  bool record(const drop::hotmethod::TaskDesc& task,
              const std::string& output_dir,
              std::string* error_msg) override;

  std::vector<std::string> collect_result() const override;

  uint32_t profiler_type() const override;

private:
  bool runPerf(int hz, const std::string& callgraph,
               const std::string& event, int pid, int duration,
               const std::string& perf_data_path,
               const std::string& output_dir, int timeout_sec,
               std::string* error_msg);

  std::vector<std::string> collected_files_;
};

}  // namespace drop
