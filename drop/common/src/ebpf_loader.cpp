#include "ebpf_loader.h"

#include <glog/logging.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace drop {

// Stack key must match the BPF program definition
struct stack_key {
  uint32_t pid;
  uint32_t tgid;
  int user_stack_id;
  int kern_stack_id;
};

struct EbpfLoader::Impl {
  struct bpf_object* obj = nullptr;
  struct bpf_program* prog = nullptr;
  struct bpf_link* link = nullptr;
  struct bpf_map* counts_map = nullptr;
  struct bpf_map* stack_map = nullptr;
  struct bpf_map* target_pid_map = nullptr;
  int counts_fd = -1;
  int stack_fd = -1;
  int target_pid_fd = -1;
  bool loaded = false;
  bool attached = false;
};

EbpfLoader::EbpfLoader() : impl_(new Impl()) {}

EbpfLoader::~EbpfLoader() {
  detach();
  unload();
  delete impl_;
}

static int libbpf_print_fn(enum libbpf_print_level level,
                            const char* format,
                            va_list args) {
  if (level == LIBBPF_DEBUG)
    return 0;
  char buf[512];
  vsnprintf(buf, sizeof(buf), format, args);
  // Trim trailingnewline
  size_t len = strlen(buf);
  while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
    buf[--len] = '\0';
  if (level == LIBBPF_WARN || level == LIBBPF_INFO)
    LOG(INFO) << "libbpf: " << buf;
  else
    LOG(WARNING) << "libbpf: " << buf;
  return 0;
}

bool EbpfLoader::load(const std::string& bpf_o_path, std::string* error_msg) {
  if (impl_->loaded) {
    *error_msg = "BPF object already loaded";
    return false;
  }

  libbpf_set_print(libbpf_print_fn);

  // Open and load the BPF object
  impl_->obj = bpf_object__open(bpf_o_path.c_str());
  if (!impl_->obj) {
    *error_msg = "Failed to open BPF object: " + bpf_o_path + " (" + std::string(strerror(errno)) + ")";
    return false;
  }

  if (bpf_object__load(impl_->obj) != 0) {
    *error_msg = "Failed to load BPF object: " + std::string(strerror(errno));
    bpf_object__close(impl_->obj);
    impl_->obj = nullptr;
    return false;
  }

  // Find the program
  impl_->prog = bpf_object__find_program_by_name(impl_->obj, "trace_sched_switch");
  if (!impl_->prog) {
    *error_msg = "Failed to find BPF program 'trace_sched_switch'";
    bpf_object__close(impl_->obj);
    impl_->obj = nullptr;
    return false;
  }

  // Find the maps
  impl_->counts_map = bpf_object__find_map_by_name(impl_->obj, "counts");
  impl_->stack_map = bpf_object__find_map_by_name(impl_->obj, "stack_traces");
  impl_->target_pid_map = bpf_object__find_map_by_name(impl_->obj, "target_pid");

  if (!impl_->counts_map || !impl_->stack_map) {
    *error_msg = "Failed to find required BPF maps (counts, stack_traces)";
    bpf_object__close(impl_->obj);
    impl_->obj = nullptr;
    return false;
  }

  impl_->counts_fd = bpf_map__fd(impl_->counts_map);
  impl_->stack_fd = bpf_map__fd(impl_->stack_map);
  if (impl_->target_pid_map)
    impl_->target_pid_fd = bpf_map__fd(impl_->target_pid_map);

  impl_->loaded = true;
  LOG(INFO) << "EbpfLoader: loaded " << bpf_o_path;
  return true;
}

bool EbpfLoader::attach(int target_pid, std::string* error_msg) {
  if (!impl_->loaded) {
    *error_msg = "BPF object not loaded";
    return false;
  }
  if (impl_->attached) {
    *error_msg = "Already attached";
    return false;
  }

  // Set PID filter if configured and map exists
  if (target_pid > 0 && impl_->target_pid_fd >= 0) {
    uint32_t key = 0;
    uint32_t val = static_cast<uint32_t>(target_pid);
    if (bpf_map_update_elem(impl_->target_pid_fd, &key, &val, BPF_ANY) != 0) {
      *error_msg = "Failed to set target PID filter: " + std::string(strerror(errno));
      return false;
    }
    LOG(INFO) << "EbpfLoader: filtering for PID " << target_pid;
  }

  // Attach the tracepoint
  impl_->link = bpf_program__attach(impl_->prog);
  if (!impl_->link) {
    *error_msg = "Failed to attach BPF program: " + std::string(strerror(errno));
    return false;
  }

  impl_->attached = true;
  LOG(INFO) << "EbpfLoader: attached tracepoint/sched/sched_switch";
  return true;
}

// Read /proc/kallsyms to build kernel symbol table
static std::map<uint64_t, std::string> ReadKallsyms() {
  std::map<uint64_t, std::string> syms;
  std::ifstream f("/proc/kallsyms");
  if (!f.is_open()) {
    LOG(WARNING) << "Cannot read /proc/kallsyms";
    return syms;
  }
  std::string line;
  while (std::getline(f, line)) {
    // Format: addr type name [module]
    std::istringstream iss(line);
    uint64_t addr;
    char type;
    std::string name;
    if (iss >> std::hex >> addr >> type >> name) {
      syms[addr] = name;
    }
  }
  return syms;
}

// Resolve a kernel address to a symbol name
static std::string ResolveKsym(const std::map<uint64_t, std::string>& syms,
                                uint64_t addr) {
  // Find the largest key <= addr
  auto it = syms.upper_bound(addr);
  if (it == syms.begin())
    return "[unknown]";
  --it;
  return it->second;
}

// User-space symbol: one entry per memory-mapped file
struct UserSym {
  uint64_t start;
  uint64_t end;
  uint64_t offset;  // file offset
  std::string path; // e.g. /usr/lib/libc.so.6
};

// Read memory mappings for a PID. Caches by PID.
static std::map<uint32_t, std::vector<UserSym>> g_user_maps_cache;
static std::mutex g_user_maps_mu;

static std::vector<UserSym> ReadUserMaps(uint32_t pid) {
  {
    std::lock_guard<std::mutex> lk(g_user_maps_mu);
    auto it = g_user_maps_cache.find(pid);
    if (it != g_user_maps_cache.end())
      return it->second;
  }

  std::vector<UserSym> result;
  std::string maps_path = "/proc/" + std::to_string(pid) + "/maps";
  std::ifstream f(maps_path);
  if (!f.is_open()) return result;

  std::string line;
  while (std::getline(f, line)) {
    // Format: start-end perms offset dev inode pathname
    UserSym sym = {};
    char perms[5] = {};
    uint64_t inode = 0;
    char dev[8] = {};
    auto idx = line.find('/');
    if (idx == std::string::npos) continue;

    // Parse the range and offset before the path
    sscanf(line.c_str(), "%lx-%lx %4s %lx %7s %lu",
           &sym.start, &sym.end, perms, &sym.offset, dev, &inode);
    sym.path = line.substr(idx);
    while (!sym.path.empty() && (sym.path.back() == '\n' || sym.path.back() == '\r'))
      sym.path.pop_back();
    if (perms[0] != 'r') continue; // skip non-readable mappings
    // Only keep executable mappings (likely code)
    if (perms[2] != 'x') continue;
    result.push_back(sym);
  }

  std::lock_guard<std::mutex> lk(g_user_maps_mu);
  g_user_maps_cache[pid] = result;
  return result;
}

// Resolve user-space address to a symbol-like string
static std::string ResolveUserFrame(uint32_t pid, uint64_t addr) {
  // Try addr2line for best symbol resolution (available on most Linux systems)
  auto maps = ReadUserMaps(pid);
  for (auto& m : maps) {
    if (addr >= m.start && addr < m.end) {
      uint64_t file_offset = addr - m.start + m.offset;
      auto slash = m.path.rfind('/');
      std::string basename = (slash != std::string::npos)
          ? m.path.substr(slash + 1) : m.path;
      // Optionally use addr2line for proper symbol names
      char cmd[256];
      snprintf(cmd, sizeof(cmd), "addr2line -e %s -f 0x%lx 2>/dev/null",
               m.path.c_str(), (unsigned long)file_offset);
      FILE* pipe = popen(cmd, "r");
      if (pipe) {
        char buf[256];
        std::string func_name;
        if (fgets(buf, sizeof(buf), pipe)) {
          func_name = buf;
          while (!func_name.empty() && (func_name.back() == '\n' || func_name.back() == '\r'))
            func_name.pop_back();
        }
        pclose(pipe);
        if (!func_name.empty() && func_name != "??" && func_name != "?") {
          return func_name;
        }
      }
      // Fallback: basename+offset
      char buf[128];
      snprintf(buf, sizeof(buf), "%s+0x%lx", basename.c_str(),
               (unsigned long)file_offset);
      return std::string(buf);
    }
  }
  char buf[64];
  snprintf(buf, sizeof(buf), "[unknown]:0x%lx", (unsigned long)addr);
  return std::string(buf);
}

bool EbpfLoader::poll(int duration_sec, const std::string& output_path,
                       std::string* error_msg) {
  if (!impl_->attached) {
    *error_msg = "Not attached";
    return false;
  }

  LOG(INFO) << "EbpfLoader: polling for " << duration_sec << " seconds...";

  // Wait for data collection
  std::this_thread::sleep_for(std::chrono::seconds(duration_sec));

  // Read kernel symbol table
  auto ksyms = ReadKallsyms();

  // Iterate over counts map and build folded stacks
  // Aggregated output: stack_string → total_count
  std::map<std::string, uint64_t> folded;

  stack_key key = {};
  struct bpf_map_iter* iter = nullptr;
  // We iterate by manually getting all keys
  uint32_t next_key = 0;
  std::vector<stack_key> keys;

  // Get all keys from the counts map
  stack_key cur_key = {};
  int fd = impl_->counts_fd;
  void* key_ptr = &cur_key;

  // Use bpf_map_get_next_key to iterate
  while (true) {
    stack_key next_key_val = {};
    int err = bpf_map_get_next_key(fd, key_ptr, &next_key_val);
    if (err) break;

    // Look up the value for this key
    uint64_t count = 0;
    err = bpf_map_lookup_elem(fd, &next_key_val, &count);
    if (err) {
      key_ptr = &next_key_val;
      continue;
    }

    if (count > 0) {
      // Build the folded stack string
      std::string stack_str;

      // Get kernel stack frames
      if (next_key_val.kern_stack_id >= 0) {
        uint64_t kstack[64] = {};
        int stack_id = next_key_val.kern_stack_id;
        if (bpf_map_lookup_elem(impl_->stack_fd, &stack_id, kstack) == 0) {
          // Kernel stack frames come in reverse order (deepest first)
          // We reverse to get top-down order for flame graph
          std::vector<std::string> frames;
          for (int i = 0; i < 64 && kstack[i] != 0; i++) {
            frames.push_back(ResolveKsym(ksyms, kstack[i]));
          }
          // Reverse: bottom of stack first
          std::reverse(frames.begin(), frames.end());
          for (auto& f : frames) {
            if (!stack_str.empty()) stack_str += ";";
            stack_str += f;
          }
        }
      }

      // Get user stack frames
      if (next_key_val.user_stack_id >= 0) {
        uint64_t ustack[64] = {};
        int stack_id = next_key_val.user_stack_id;
        if (bpf_map_lookup_elem(impl_->stack_fd, &stack_id, ustack) == 0) {
          std::vector<std::string> frames;
          for (int i = 0; i < 64 && ustack[i] != 0; i++) {
            frames.push_back(ResolveUserFrame(next_key_val.tgid, ustack[i]));
          }
          std::reverse(frames.begin(), frames.end());
          for (auto& f : frames) {
            if (!stack_str.empty()) stack_str += ";";
            stack_str += f;
          }
        }
      }

      if (stack_str.empty()) {
        stack_str = "[unknown]";
      }

      folded[stack_str] += count;
    }

    // Move to next key
    cur_key = next_key_val;
    key_ptr = &cur_key;
  }

  // Write folded stack output
  std::ofstream out(output_path);
  if (!out.is_open()) {
    *error_msg = "Failed to open output file: " + output_path;
    return false;
  }

  for (auto& [stack, count] : folded) {
    out << stack << " " << count << "\n";
  }
  out.close();

  LOG(INFO) << "EbpfLoader: wrote " << folded.size() << " stacks to " << output_path;
  return true;
}

void EbpfLoader::detach() {
  if (impl_->link) {
    bpf_link__destroy(impl_->link);
    impl_->link = nullptr;
    LOG(INFO) << "EbpfLoader: detached";
  }
  impl_->attached = false;
}

void EbpfLoader::unload() {
  if (impl_->obj) {
    bpf_object__close(impl_->obj);
    impl_->obj = nullptr;
    LOG(INFO) << "EbpfLoader: unloaded";
  }
  impl_->loaded = false;
  impl_->counts_fd = -1;
  impl_->stack_fd = -1;
  impl_->target_pid_fd = -1;
}

}  // namespace drop
