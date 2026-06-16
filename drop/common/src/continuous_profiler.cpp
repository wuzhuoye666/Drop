#include "continuous_profiler.h"
#include "profiler_factory.h"
#include "ebpf_profiler.h"
#include "perf_profiler.h"
#include "async_profiler_profiler.h"

#include <glog/logging.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace drop {

ContinuousProfiler::ContinuousProfiler() = default;
ContinuousProfiler::~ContinuousProfiler() = default;

bool ContinuousProfiler::record(const drop::hotmethod::TaskDesc& task,
                                 const std::string& output_dir,
                                 std::string* error_msg) {
  const auto& argv = task.sample_argv();
  int pid = argv.pid();
  int hz = argv.hz();
  if (hz <= 0) hz = 1;  // continuous default: 1 Hz low-frequency

  // Determine which underlying profiler to use based on profiler_type
  uint32_t inner_type = task.profiler_type();
  // For continuous profiling, we default to perf (0) if not specified
  if (inner_type == 0 && task.task_type() == 1) {
    // Keep as perf
  }

  LOG(INFO) << "Continuous profiling mode started, hz=" << hz
            << " profiler_type=" << inner_type
            << " pid=" << pid;

  std::filesystem::create_directories(output_dir);

  segment_start_ts_ = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  while (!stop_requested_.load()) {
    // Build a sub-task descriptor for the short burst
    drop::hotmethod::TaskDesc burst_task;
    burst_task.set_task_id(task.task_id());
    burst_task.set_task_type(0);  // single-shot for the burst
    burst_task.set_profiler_type(inner_type);
    burst_task.mutable_sample_argv()->CopyFrom(argv);
    burst_task.mutable_sample_argv()->set_duration(kSampleBurstSeconds);
    burst_task.mutable_sample_argv()->set_hz(hz);
    if (task.timeout_sec() > 0) {
      burst_task.set_timeout_sec(kSampleBurstSeconds + 30);
    }

    // Create an inner profiler for the burst
    auto inner = drop::CreateProfiler(inner_type);
    if (!inner) {
      *error_msg = "unsupported inner profiler_type=" + std::to_string(inner_type);
      return false;
    }

    // Run the short burst
    std::string burst_error;
    bool ok = inner->record(burst_task, output_dir, &burst_error);
    if (!ok) {
      LOG(WARNING) << "Continuous burst failed: " << burst_error
                   << " (continuing)";
    } else {
      // Merge the collected collapsed stack file(s) into our in-memory map
      for (const auto& fname : inner->collect_result()) {
        std::string fpath = output_dir + "/" + fname;
        if (fname.find("collapsed") != std::string::npos &&
            std::filesystem::exists(fpath)) {
          merge_collapsed(fpath);
        }
        // Clean up burst output files to save disk space
        std::filesystem::remove(fpath);
      }
    }

    // Check if it's time to flush a segment
    auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (now_ts - segment_start_ts_ >= kSegmentSeconds) {
      std::string flush_error;
      if (!flush_segment(task.task_id(), output_dir, &flush_error)) {
        LOG(WARNING) << "Segment flush failed: " << flush_error;
      }
      // Reset for next segment
      segment_start_ts_ = now_ts;
    }

    // Brief sleep between bursts to avoid busy-looping
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  // Final flush on stop
  if (!stacks_.empty()) {
    std::string flush_error;
    flush_segment(task.task_id(), output_dir, &flush_error);
  }

  LOG(INFO) << "Continuous profiling stopped for tid=" << task.task_id();
  return true;
}

std::vector<std::string> ContinuousProfiler::collect_result() const {
  std::lock_guard<std::mutex> lk(stacks_mu_);
  return flushed_files_;
}

uint32_t ContinuousProfiler::profiler_type() const {
  // Continuous profiler is a wrapper; the actual profiler_type depends
  // on the inner profiler. Return a sentinel value.
  return 0;
}

void ContinuousProfiler::stop() {
  stop_requested_.store(true);
}

void ContinuousProfiler::merge_collapsed(const std::string& filepath) {
  std::ifstream f(filepath);
  if (!f.is_open()) return;

  std::string line;
  std::lock_guard<std::mutex> lk(stacks_mu_);
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    // Format: stack_frame;stack_frame;... count
    auto last_space = line.rfind(' ');
    if (last_space == std::string::npos) continue;
    std::string stack = line.substr(0, last_space);
    uint64_t count = 0;
    try { count = std::stoull(line.substr(last_space + 1)); } catch (...) { continue; }
    stacks_[stack] += count;
  }
}

bool ContinuousProfiler::flush_segment(const std::string& tid,
                                        const std::string& output_dir,
                                        std::string* error_msg) {
  auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  // Write the accumulated stacks to a collapsed file
  std::string ts_str = std::to_string(segment_start_ts_);
  std::string seg_dir = output_dir + "/" + ts_str;
  std::filesystem::create_directories(seg_dir);
  std::string seg_file = seg_dir + "/collapsed.txt";

  {
    std::ofstream out(seg_file, std::ios::trunc);
    if (!out.is_open()) {
      *error_msg = "cannot create segment file: " + seg_file;
      return false;
    }
    std::lock_guard<std::mutex> lk(stacks_mu_);
    for (const auto& [stack, count] : stacks_) {
      out << stack << " " << count << "\n";
    }
    stacks_.clear();
  }

  LOG(INFO) << "Flushed segment " << ts_str << " for tid=" << tid
            << " size=" << std::filesystem::file_size(seg_file);

  // Upload segment file to MinIO via apiserver
  std::string cos_key;
  if (!upload_segment(tid, seg_file, &cos_key)) {
    *error_msg = "upload failed for segment " + ts_str;
    return false;
  }

  // Register the segment via apiserver
  if (!register_segment(tid, segment_start_ts_, now_ts, cos_key)) {
    *error_msg = "register segment failed for " + ts_str;
    return false;
  }

  {
    std::lock_guard<std::mutex> lk(stacks_mu_);
    flushed_files_.push_back(ts_str + "/collapsed.txt");
  }

  return true;
}

bool ContinuousProfiler::upload_segment(const std::string& tid,
                                         const std::string& filepath,
                                         std::string* cos_key) {
  // Extract the relative path for the upload URL
  // The file is at <output_dir>/<timestamp>/collapsed.txt
  // We want the upload path to be <tid>/<timestamp>/collapsed.txt
  auto last_slash = filepath.rfind('/');
  auto second_slash = filepath.rfind('/', last_slash - 1);
  std::string relative_path;
  if (second_slash != std::string::npos) {
    relative_path = filepath.substr(second_slash + 1);
  } else {
    relative_path = filepath.substr(last_slash + 1);
  }

  std::string url = apiserver_addr_ + "/api/v1/tasks/" + tid + "/upload/" + relative_path;

  std::string cmd = "curl -sf -o /dev/null -w '%{http_code}' "
                    "-F 'file=@" + filepath + "' "
                    "'" + url + "' 2>/dev/null";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    *cos_key = "popen failed";
    return false;
  }

  char buf[64];
  std::string result;
  while (fgets(buf, sizeof(buf), pipe)) result += buf;
  int rc = pclose(pipe);

  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();

  int http_code = 0;
  try { http_code = std::stoi(result); } catch (...) {}

  if (rc != 0 || http_code != 200) {
    *cos_key = "HTTP " + result;
    return false;
  }

  *cos_key = tid + "/" + relative_path;
  return true;
}

bool ContinuousProfiler::register_segment(const std::string& tid,
                                           int64_t start_ts,
                                           int64_t end_ts,
                                           const std::string& s3_key) {
  // POST /api/v1/tasks/<tid>/segments
  std::string url = apiserver_addr_ + "/api/v1/tasks/" + tid + "/segments";
  std::string body = "{\"start_ts\":" + std::to_string(start_ts) +
                     ",\"end_ts\":" + std::to_string(end_ts) +
                     ",\"s3_key\":\"" + s3_key + "\"}";

  std::string cmd = "curl -sf -o /dev/null -w '%{http_code}' "
                    "-H 'Content-Type: application/json' "
                    "-d '" + body + "' "
                    "'" + url + "' 2>/dev/null";

  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return false;

  char buf[64];
  std::string result;
  while (fgets(buf, sizeof(buf), pipe)) result += buf;
  int rc = pclose(pipe);

  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();

  int http_code = 0;
  try { http_code = std::stoi(result); } catch (...) {}

  if (rc != 0 || http_code != 200) {
    LOG(WARNING) << "Register segment HTTP " << result;
    return false;
  }

  return true;
}

}  // namespace drop
