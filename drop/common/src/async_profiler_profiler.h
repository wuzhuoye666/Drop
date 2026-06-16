#pragma once

#include "profiler.h"

#include <string>
#include <vector>

namespace drop {

// Java async-profiler profiler.
// Executes: asprof -d <duration> -f <output>.collapsed [-e event] [-i interval] <pid>
// Produces a collapsed stack file compatible with flamegraph.pl --color java.
class AsyncProfilerProfiler : public IProfiler {
public:
  bool record(const drop::hotmethod::TaskDesc& task,
              const std::string& output_dir,
              std::string* error_msg) override;

  std::vector<std::string> collect_result() const override;

  uint32_t profiler_type() const override;

private:
  // Locate the asprof binary. Searches several standard paths.
  static std::string findAsprof();

  std::vector<std::string> collected_files_;
};

}  // namespace drop
