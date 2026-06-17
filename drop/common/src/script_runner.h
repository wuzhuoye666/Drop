#pragma once

#include "profiler.h"

#include <string>
#include <vector>

namespace drop {

// Script executor profiler (profiler_type=5).
// Writes task.script_content() to a temp .sh file, then executes
// /bin/sh <script> with fork+execvp, setpgid, and watchdog timeout.
// Captures stdout to script_output.txt in the output directory.
// Follows the same fork/exec/timeout pattern as PerfProfiler.
class ScriptRunner : public IProfiler {
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
