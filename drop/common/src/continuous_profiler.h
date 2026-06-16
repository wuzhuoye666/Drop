#pragma once

#include "profiler.h"

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <chrono>

namespace drop {

// Continuous profiler: wraps a regular profiler (perf or eBPF) in a
// long-running loop with periodic segment flushing.
//
// Instead of running one continuous perf record for hours, we run
// short-duration sampling bursts (default 30s) in a loop and accumulate
// folded stacks in an in-memory map. Every 5 minutes we flush the
// accumulated data to a collapsed stack file, upload it, and register
// the segment.
class ContinuousProfiler : public IProfiler {
public:
  ContinuousProfiler();
  ~ContinuousProfiler() override;

  // Blocks indefinitely (until stop() is called from another thread).
  // Each iteration runs a short perf/ebpf collection burst, folds the
  // results into the in-memory map, and periodically flushes segments.
  bool record(const drop::hotmethod::TaskDesc& task,
              const std::string& output_dir,
              std::string* error_msg) override;

  // Returns the list of segment files flushed so far.
  std::vector<std::string> collect_result() const override;

  uint32_t profiler_type() const override;

  // Signal the profiler to stop after the current burst.
  void stop();

  // Accessor for the APIserver upload endpoint — set before record().
  void set_apiserver_addr(const std::string& addr) { apiserver_addr_ = addr; }

private:
  // Flush the current ring buffer to a collapsed stack file and upload.
  bool flush_segment(const std::string& tid,
                     const std::string& output_dir,
                     std::string* error_msg);

  // Parse a collapsed-stack file and merge into the in-memory map.
  void merge_collapsed(const std::string& filepath);

  // Upload a file segment to apiserver and return the cos_key.
  bool upload_segment(const std::string& tid,
                      const std::string& filepath,
                      std::string* cos_key);

  // Register a segment via apiserver.
  bool register_segment(const std::string& tid,
                        int64_t start_ts,
                        int64_t end_ts,
                        const std::string& s3_key);

  std::map<std::string, uint64_t> stacks_;   // folded_stack → count
  mutable std::mutex stacks_mu_;
  std::atomic<bool> stop_requested_{false};

  int64_t segment_start_ts_ = 0;  // epoch seconds of current segment
  static constexpr int kSegmentSeconds = 300;  // 5 minutes
  static constexpr int kSampleBurstSeconds = 30;  // each burst duration

  std::string apiserver_addr_;
  std::vector<std::string> flushed_files_;
};

}  // namespace drop
