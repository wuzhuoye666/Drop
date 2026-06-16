#include "ebpf_profiler.h"
#include "ebpf_loader.h"

#include <glog/logging.h>

#include <filesystem>
#include <string>

namespace drop {

bool EbpfProfiler::record(const drop::hotmethod::TaskDesc& task,
                            const std::string& output_dir,
                            std::string* error_msg) {
  const auto& argv = task.sample_argv();
  int duration = argv.duration();
  if (duration <= 0) duration = 10;
  int pid = argv.pid();

  // Locate the pre-compiled BPF object
  // Search order: alongside binary, /opt/drop/bpf, source tree
  std::string bpf_o_path;
  std::vector<std::string> candidates = {
      "/opt/drop/bpf/offcpu.bpf.o",
      // Relative to the executable — resolved at runtime
      std::filesystem::current_path() / "common" / "bpf" / "offcpu.bpf.o",
      "/root/Drop/drop/common/bpf/offcpu.bpf.o",
  };

  // Also check alongside the running binary
  char exe_path[4096] = {};
  ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len > 0) {
    std::string exe_dir = std::filesystem::path(exe_path).parent_path().string();
    candidates.insert(candidates.begin(), exe_dir + "/../common/bpf/offcpu.bpf.o");
  }

  for (auto& c : candidates) {
    if (std::filesystem::exists(c)) {
      bpf_o_path = c;
      break;
    }
  }

  if (bpf_o_path.empty()) {
    *error_msg = "Cannot find offcpu.bpf.o (searched standard paths)";
    return false;
  }

  LOG(INFO) << "EbpfProfiler: using BPF object " << bpf_o_path;

  EbpfLoader loader;
  if (!loader.load(bpf_o_path, error_msg)) {
    return false;
  }

  if (!loader.attach(pid, error_msg)) {
    return false;
  }

  // Create output directory
  std::filesystem::create_directories(output_dir);

  std::string output_path = output_dir + "/collapsed_ebpf.txt";
  LOG(INFO) << "EbpfProfiler: started for tid=" << task.task_id()
            << " pid=" << pid << " duration=" << duration;

  bool ok = loader.poll(duration, output_path, error_msg);

  loader.detach();
  loader.unload();

  if (ok) {
    collected_files_ = {"collapsed_ebpf.txt"};
    LOG(INFO) << "EbpfProfiler: collected " << output_path
              << " size=" << std::filesystem::file_size(output_path);
  }

  return ok;
}

std::vector<std::string> EbpfProfiler::collect_result() const {
  return collected_files_;
}

uint32_t EbpfProfiler::profiler_type() const {
  return 3;  // ebpf
}

}  // namespace drop
