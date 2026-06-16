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
#include <deque>
#include <set>

namespace drop {

// Stack key must match the BPF program definition
struct stack_key {
  uint32_t pid;
  uint32_t tgid;
  int user_stack_id;
  int kern_stack_id;

  // Required for std::map key
  bool operator<(const stack_key& o) const {
    if (pid != o.pid) return pid < o.pid;
    if (tgid != o.tgid) return tgid < o.tgid;
    if (user_stack_id != o.user_stack_id) return user_stack_id < o.user_stack_id;
    return kern_stack_id < o.kern_stack_id;
  }
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

  impl_->prog = bpf_object__find_program_by_name(impl_->obj, "trace_sched_switch");
  if (!impl_->prog) {
    *error_msg = "Failed to find BPF program 'trace_sched_switch'";
    bpf_object__close(impl_->obj);
    impl_->obj = nullptr;
    return false;
  }

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

  if (target_pid > 0 && impl_->target_pid_fd >= 0) {
    uint32_t key = 0;
    uint32_t val = static_cast<uint32_t>(target_pid);
    if (bpf_map_update_elem(impl_->target_pid_fd, &key, &val, BPF_ANY) != 0) {
      *error_msg = "Failed to set target PID filter: " + std::string(strerror(errno));
      return false;
    }
    LOG(INFO) << "EbpfLoader: filtering for PID " << target_pid;
  }

  impl_->link = bpf_program__attach(impl_->prog);
  if (!impl_->link) {
    *error_msg = "Failed to attach BPF program: " + std::string(strerror(errno));
    return false;
  }

  impl_->attached = true;
  LOG(INFO) << "EbpfLoader: attached tracepoint/sched/sched_switch";
  return true;
}

// ── Kernel symbol resolution ────────────────────────────────────────

static std::map<uint64_t, std::string> ReadKallsyms() {
  std::map<uint64_t, std::string> syms;
  std::ifstream f("/proc/kallsyms");
  if (!f.is_open()) {
    LOG(WARNING) << "Cannot read /proc/kallsyms";
    return syms;
  }
  std::string line;
  while (std::getline(f, line)) {
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

static std::string ResolveKsym(const std::map<uint64_t, std::string>& syms,
                                uint64_t addr) {
  auto it = syms.upper_bound(addr);
  if (it == syms.begin())
    return "[unknown]";
  --it;
  return it->second;
}

// ── User-space symbol resolution (batched addr2line) ────────────────

struct UserSym {
  uint64_t start;
  uint64_t end;
  uint64_t offset;
  std::string path;
};

// Cache with bounded size and TTL-style eviction
static constexpr size_t kMaxUserMapsCache = 256;
static std::map<uint32_t, std::vector<UserSym>> g_user_maps_cache;
static std::deque<uint32_t> g_user_maps_lru;
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
    UserSym sym = {};
    char perms[5] = {};
    uint64_t inode = 0;
    char dev[8] = {};
    auto idx = line.find('/');
    if (idx == std::string::npos) continue;

    sscanf(line.c_str(), "%lx-%lx %4s %lx %7s %lu",
           &sym.start, &sym.end, perms, &sym.offset, dev, &inode);
    sym.path = line.substr(idx);
    while (!sym.path.empty() && (sym.path.back() == '\n' || sym.path.back() == '\r'))
      sym.path.pop_back();
    if (perms[0] != 'r' || perms[2] != 'x') continue;
    result.push_back(sym);
  }

  std::lock_guard<std::mutex> lk(g_user_maps_mu);
  // Evict oldest entry if cache is full
  if (g_user_maps_cache.size() >= kMaxUserMapsCache && !g_user_maps_lru.empty()) {
    uint32_t oldest = g_user_maps_lru.front();
    g_user_maps_lru.pop_front();
    g_user_maps_cache.erase(oldest);
  }
  g_user_maps_cache[pid] = result;
  g_user_maps_lru.push_back(pid);
  return result;
}

// Batch resolve user-space symbols using a single addr2line invocation per binary.
// Collects all (path, file_offset) pairs first, groups by path, then calls
// addr2line once per binary with all offsets as arguments.
static std::map<std::pair<std::string, uint64_t>, std::string>
BatchResolveUserSymbols(
    const std::vector<std::pair<std::string, uint64_t>>& to_resolve) {
  std::map<std::pair<std::string, uint64_t>, std::string> result;

  // Group by binary path
  std::map<std::string, std::vector<uint64_t>> by_path;
  for (auto& [path, offset] : to_resolve) {
    by_path[path].push_back(offset);
  }

  for (auto& [path, offsets] : by_path) {
    // Build addr2line command with all offsets
    // Limit to 512 addresses per invocation to avoid ARG_MAX
    size_t start = 0;
    while (start < offsets.size()) {
      size_t end = std::min(start + 512, offsets.size());
      std::string cmd = "addr2line -e " + path + " -f";
      for (size_t i = start; i < end; ++i) {
        char buf[32];
        snprintf(buf, sizeof(buf), " 0x%lx", (unsigned long)offsets[i]);
        cmd += buf;
      }
      cmd += " 2>/dev/null";

      FILE* pipe = popen(cmd.c_str(), "r");
      if (!pipe) {
        // Fallback: basename+offset for all in this batch
        auto slash = path.rfind('/');
        std::string basename = (slash != std::string::npos)
            ? path.substr(slash + 1) : path;
        for (size_t i = start; i < end; ++i) {
          char buf[128];
          snprintf(buf, sizeof(buf), "%s+0x%lx", basename.c_str(),
                   (unsigned long)offsets[i]);
          result[{path, offsets[i]}] = buf;
        }
        start = end;
        continue;
      }

      // addr2line -f outputs two lines per address: function name, then file:line
      char buf[512];
      for (size_t i = start; i < end; ++i) {
        std::string func_name;
        if (fgets(buf, sizeof(buf), pipe)) {
          func_name = buf;
          while (!func_name.empty() && (func_name.back() == '\n' || func_name.back() == '\r'))
            func_name.pop_back();
        }
        // Skip the file:line line
        (void)fgets(buf, sizeof(buf), pipe);

        if (!func_name.empty() && func_name != "??" && func_name != "?") {
          result[{path, offsets[i]}] = func_name;
        } else {
          // Fallback to basename+offset
          auto slash = path.rfind('/');
          std::string basename = (slash != std::string::npos)
              ? path.substr(slash + 1) : path;
          char fbuf[128];
          snprintf(fbuf, sizeof(fbuf), "%s+0x%lx", basename.c_str(),
                   (unsigned long)offsets[i]);
          result[{path, offsets[i]}] = fbuf;
        }
      }
      pclose(pipe);
      start = end;
    }
  }
  return result;
}

// Single-frame resolution (for the non-batched code path)
static std::string ResolveUserFrame(
    uint32_t pid, uint64_t addr,
    const std::map<std::pair<std::string, uint64_t>, std::string>& resolved) {
  auto maps = ReadUserMaps(pid);
  for (auto& m : maps) {
    if (addr >= m.start && addr < m.end) {
      uint64_t file_offset = addr - m.start + m.offset;
      auto it = resolved.find({m.path, file_offset});
      if (it != resolved.end()) return it->second;
      // Fallback
      auto slash = m.path.rfind('/');
      std::string basename = (slash != std::string::npos)
          ? m.path.substr(slash + 1) : m.path;
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

// ── Poll: collect, resolve, cleanup ─────────────────────────────────

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

  // ── Phase 1: collect all counts keys and stack data ─────────────
  std::vector<stack_key> all_keys;
  std::map<stack_key, uint64_t> key_counts;
  std::set<int> used_stack_ids;

  {
    stack_key prev_key = {};
    bool first = true;
    while (true) {
      stack_key next_key = {};
      void* prev_ptr = first ? nullptr : &prev_key;
      int err = bpf_map_get_next_key(impl_->counts_fd, prev_ptr, &next_key);
      if (err) break;
      first = false;

      uint64_t count = 0;
      if (bpf_map_lookup_elem(impl_->counts_fd, &next_key, &count) != 0 || count == 0) {
        prev_key = next_key;
        continue;
      }

      all_keys.push_back(next_key);
      key_counts[next_key] = count;

      // Track used stack IDs for cleanup
      if (next_key.kern_stack_id >= 0) used_stack_ids.insert(next_key.kern_stack_id);
      if (next_key.user_stack_id >= 0) used_stack_ids.insert(next_key.user_stack_id);

      prev_key = next_key;
    }
  }

  // ── Phase 2: collect all user addresses for batch resolution ────
  std::vector<std::pair<std::string, uint64_t>> to_resolve;

  // Map from (pid, user_stack_id) → vector of frame addresses
  // We need to collect them first, then batch-resolve
  std::map<std::pair<uint32_t, int>, std::vector<uint64_t>> user_stacks;

  for (auto& key : all_keys) {
    if (key.user_stack_id >= 0) {
      uint64_t ustack[64] = {};
      int stack_id = key.user_stack_id;
      if (bpf_map_lookup_elem(impl_->stack_fd, &stack_id, ustack) == 0) {
        std::vector<uint64_t> addrs;
        for (int i = 0; i < 64 && ustack[i] != 0; i++) {
          addrs.push_back(ustack[i]);
        }
        user_stacks[{key.tgid, key.user_stack_id}] = addrs;

        // Collect (path, file_offset) for batch resolution
        auto maps = ReadUserMaps(key.tgid);
        for (auto addr : addrs) {
          for (auto& m : maps) {
            if (addr >= m.start && addr < m.end) {
              uint64_t file_offset = addr - m.start + m.offset;
              to_resolve.push_back({m.path, file_offset});
              break;
            }
          }
        }
      }
    }
  }

  // Batch resolve all user symbols
  auto resolved_syms = BatchResolveUserSymbols(to_resolve);

  // ── Phase 3: build folded stacks ────────────────────────────────
  std::map<std::string, uint64_t> folded;

  for (auto& key : all_keys) {
    uint64_t count = key_counts[key];
    std::string stack_str;

    // Kernel stack frames
    if (key.kern_stack_id >= 0) {
      uint64_t kstack[64] = {};
      int stack_id = key.kern_stack_id;
      if (bpf_map_lookup_elem(impl_->stack_fd, &stack_id, kstack) == 0) {
        std::vector<std::string> frames;
        for (int i = 0; i < 64 && kstack[i] != 0; i++) {
          frames.push_back(ResolveKsym(ksyms, kstack[i]));
        }
        std::reverse(frames.begin(), frames.end());
        for (auto& f : frames) {
          if (!stack_str.empty()) stack_str += ";";
          stack_str += f;
        }
      }
    }

    // User stack frames (use pre-resolved symbols)
    if (key.user_stack_id >= 0) {
      auto it = user_stacks.find({key.tgid, key.user_stack_id});
      if (it != user_stacks.end()) {
        std::vector<std::string> frames;
        for (auto addr : it->second) {
          frames.push_back(ResolveUserFrame(key.tgid, addr, resolved_syms));
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

  // ── Phase 4: write output ───────────────────────────────────────
  std::ofstream out(output_path);
  if (!out.is_open()) {
    *error_msg = "Failed to open output file: " + output_path;
    return false;
  }

  for (auto& [stack, count] : folded) {
    out << stack << " " << count << "\n";
  }
  out.close();

  // ── Phase 5: cleanup BPF maps to prevent exhaustion ─────────────
  // Delete all counts keys
  for (auto& key : all_keys) {
    bpf_map_delete_elem(impl_->counts_fd, &key);
  }

  // Delete used stack trace entries
  for (int stack_id : used_stack_ids) {
    bpf_map_delete_elem(impl_->stack_fd, &stack_id);
  }

  LOG(INFO) << "EbpfLoader: wrote " << folded.size() << " stacks to " << output_path
            << " (cleaned " << all_keys.size() << " counts + "
            << used_stack_ids.size() << " stack IDs)";
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
