#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace drop {

// EbpfLoader: loads a pre-compiled BPF object, attaches tracepoints,
// polls stack counts, and writes folded stack output.
//
// Lifecycle: load() → attach(pid) → poll(duration_sec) → detach() → unload()
class EbpfLoader {
public:
  EbpfLoader();
  ~EbpfLoader();

  // Load and open the BPF object file.
  bool load(const std::string& bpf_o_path, std::string* error_msg);

  // Attach the tracepoint, with optional PID filtering (0 = system-wide).
  bool attach(int target_pid, std::string* error_msg);

  // Collect samples for the given duration (blocking).
  // Returns the path to the output folded-stack file.
  bool poll(int duration_sec, const std::string& output_path, std::string* error_msg);

  // Detach tracepoints.
  void detach();

  // Unload BPF object and free resources.
  void unload();

private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace drop
