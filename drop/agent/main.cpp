#include <grpcpp/grpcpp.h>

#include "common.pb.h"
#include "healthcheck.grpc.pb.h"
#include "hotmethod.grpc.pb.h"
#include "init.grpc.pb.h"

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "profiler.h"
#include "profiler_factory.h"

#include <iostream>
#include <fstream>
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>

// ── gflags ──────────────────────────────────────────────────────────
DEFINE_string(config, "", "Path to agent config JSON");
DEFINE_bool(foreground, false, "Run in foreground (do not daemonize)");
DEFINE_string(server_addr, "127.0.0.1:50051", "drop_server gRPC address");
DEFINE_string(hostname, "", "Agent hostname (auto-detected if empty)");
DEFINE_string(ip, "", "Agent IP address (auto-detected if empty)");
DEFINE_string(uid, "", "Agent unique ID (auto-generated if empty)");
DEFINE_string(agent_version, "0.1.0", "Agent version string");
DEFINE_string(apiserver_addr, "http://127.0.0.1:8191", "apiserver HTTP address for file uploads");

// ── Worker state ────────────────────────────────────────────────────
struct WorkerState {
  std::deque<drop::hotmethod::TaskDesc> queue;
  std::mutex mu;
  std::condition_variable cv;
  bool shutdown = false;
};

static WorkerState g_worker;

// ── PidStats: self-monitoring ───────────────────────────────────────
static long g_clk_tck = 0;
static long g_page_size = 0;

struct ProcStats {
  uint64_t utime = 0;
  uint64_t stime = 0;
  uint64_t rss_pages = 0;
  uint64_t read_bytes = 0;
  uint64_t write_bytes = 0;
  double   cpu_percent = 0.0;
  uint64_t rss_mb = 0;
  uint64_t read_kbs = 0;
  uint64_t write_kbs = 0;
};

static bool ReadSelfStat(ProcStats* out) {
  std::ifstream f("/proc/self/stat");
  if (!f.is_open()) return false;

  std::string line;
  std::getline(f, line);
  f.close();

  // Format: pid (comm) state ppid ... utime stime ... rss ...
  // Fields are 1-indexed: utime=14, stime=15, rss=24
  // Skip comm by finding last ')'
  auto comm_end = line.rfind(')');
  if (comm_end == std::string::npos) return false;

  std::string rest = line.substr(comm_end + 2);
  int field = 3;  // after comm, next is state (field 3)
  size_t pos = 0;
  while (pos < rest.size()) {
    size_t next = rest.find(' ', pos);
    if (next == std::string::npos) next = rest.size();
    std::string tok = rest.substr(pos, next - pos);
    if (field == 14) out->utime = std::stoull(tok);
    else if (field == 15) out->stime = std::stoull(tok);
    else if (field == 24) out->rss_pages = std::stoull(tok);
    pos = next + 1;
    field++;
    if (field > 24) break;
  }

  out->rss_mb = (out->rss_pages * g_page_size) / (1024 * 1024);
  return true;
}

static bool ReadSelfIO(ProcStats* out) {
  std::ifstream f("/proc/self/io");
  if (!f.is_open()) return false;

  std::string line;
  while (std::getline(f, line)) {
    if (line.compare(0, 11, "read_bytes:") == 0) {
      out->read_bytes = std::stoull(line.substr(11));
    } else if (line.compare(0, 12, "write_bytes:") == 0) {
      out->write_bytes = std::stoull(line.substr(12));
    }
  }
  return true;
}

static drop::common::PidStats ComputePidStats() {
  static ProcStats prev = {};
  static auto prev_time = std::chrono::steady_clock::now();

  ProcStats cur;
  ReadSelfStat(&cur);
  ReadSelfIO(&cur);

  auto now = std::chrono::steady_clock::now();
  double elapsed_sec = std::chrono::duration<double>(now - prev_time).count();
  if (elapsed_sec < 0.01) elapsed_sec = 0.01;

  cur.cpu_percent = static_cast<double>((cur.utime - prev.utime) + (cur.stime - prev.stime))
                    / (elapsed_sec * g_clk_tck) * 100.0;
  cur.rss_mb = (cur.rss_pages * g_page_size) / (1024 * 1024);
  cur.read_kbs = static_cast<uint64_t>((cur.read_bytes - prev.read_bytes) / elapsed_sec / 1024);
  cur.write_kbs = static_cast<uint64_t>((cur.write_bytes - prev.write_bytes) / elapsed_sec / 1024);

  prev = cur;
  prev_time = now;

  drop::common::PidStats ps;
  ps.set_name("drop_agent");
  ps.set_pid(getpid());
  ps.set_utime(cur.utime);
  ps.set_stime(cur.stime);
  ps.set_rss(cur.rss_pages);
  ps.set_cpu_percent(cur.cpu_percent);
  ps.set_rss_mb(cur.rss_mb);
  ps.set_read_kbs(cur.read_kbs);
  ps.set_write_kbs(cur.write_kbs);

  LOG_EVERY_N(INFO, 10) << "PidStats: cpu=" << cur.cpu_percent
          << "% rss=" << cur.rss_mb << "MB"
          << " read=" << cur.read_kbs << "KB/s"
          << " write=" << cur.write_kbs << "KB/s";

  return ps;
}

// ── Upload file to apiserver via curl multipart POST ───────────────
static bool UploadFileToAPI(const std::string& tid,
                             const std::string& filepath,
                             std::string* cos_key) {
  // Build URL: POST /api/v1/tasks/<tid>/upload/<filename>
  std::string filename = filepath.substr(filepath.rfind('/') + 1);
  std::string url = FLAGS_apiserver_addr + "/api/v1/tasks/" + tid + "/upload/" + filename;

  // Use curl multipart/form-data upload
  std::string cmd = "curl -sf -o /dev/null -w '%{http_code}' "
                    "-F 'file=@" + filepath + "' "
                    "'" + url + "' 2>/dev/null";

  // Instead of system(), use popen (still spawns a shell but avoids system())
  // For production, we'd use libcurl — this is acceptable for MVP
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    *cos_key = "popen failed";
    return false;
  }

  char buf[64];
  std::string result;
  while (fgets(buf, sizeof(buf), pipe)) {
    result += buf;
  }
  int rc = pclose(pipe);

  // Trim newline
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
    result.pop_back();

  int http_code = 0;
  try { http_code = std::stoi(result); } catch (...) {}

  if (rc != 0 || http_code != 200) {
    *cos_key = "upload HTTP " + result + " (rc=" + std::to_string(rc) + ")";
    return false;
  }

  *cos_key = tid + "/" + filename;
  return true;
}

// ── Daemonize ───────────────────────────────────────────────────────
static void Daemonize() {
  // First fork
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork()");
    _exit(1);
  }
  if (pid > 0) {
    _exit(0);  // parent exits
  }

  // Create new session
  if (setsid() < 0) {
    perror("setsid()");
    _exit(1);
  }

  // Second fork
  pid = fork();
  if (pid < 0) {
    perror("fork()");
    _exit(1);
  }
  if (pid > 0) {
    _exit(0);
  }

  // Close standard file descriptors and redirect to /dev/null
  // Note: glog reopens log files by file path, so closing fd 2 is safe
  close(0);
  close(1);
  close(2);

  int devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    dup2(devnull, 0);
    dup2(devnull, 1);
    dup2(devnull, 2);
    if (devnull > 2) close(devnull);
  }

  // Write PID file
  std::ofstream pidf("/tmp/drop_agent.pid");
  if (pidf.is_open()) {
    pidf << getpid();
    pidf.close();
  }

  // Set umask
  umask(0027);
}

// ── Signal handler ──────────────────────────────────────────────────
static void SignalHandler(int sig) {
  LOG(INFO) << "Received signal " << sig << ", shutting down...";
  {
    std::lock_guard<std::mutex> lk(g_worker.mu);
    g_worker.shutdown = true;
  }
  g_worker.cv.notify_all();
}

// ── Heartbeat thread ────────────────────────────────────────────────
static void HeartbeatThread(std::shared_ptr<grpc::Channel> channel) {
  auto stub = drop::healthcheck::HealthCheck::NewStub(channel);
  auto init_stub = drop::init::Init::NewStub(channel);
  bool registered = false;

  while (true) {
    if (g_worker.shutdown) break;

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    drop::healthcheck::HealthCheckRequest req;
    req.set_host_name(FLAGS_hostname);
    req.set_ip_addr(FLAGS_ip);
    req.set_uid(FLAGS_uid);
  req.set_agent_version(FLAGS_agent_version);
  *req.mutable_self_pstats() = ComputePidStats();

    drop::healthcheck::HealthCheckResponse resp;
    auto status = stub->Do(&ctx, req, &resp);

    if (!status.ok()) {
      LOG(WARNING) << "Heartbeat failed: " << status.error_message();
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    LOG_EVERY_N(INFO, 5) << "Heartbeat OK, pending=" << resp.pending();

    // Register on first successful heartbeat
    if (!registered) {
      grpc::ClientContext reg_ctx;
      reg_ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
      drop::init::RegisterAgentRequest reg_req;
      reg_req.set_host_name(FLAGS_hostname);
      reg_req.set_ip_addr(FLAGS_ip);
      reg_req.set_uid(FLAGS_uid);
      reg_req.set_agent_version(FLAGS_agent_version);
      drop::init::RegisterAgentResponse reg_resp;
      auto reg_status = init_stub->RegisterAgent(&reg_ctx, reg_req, &reg_resp);
      if (reg_status.ok()) {
        registered = true;
        LOG(INFO) << "Agent registered with server";
      }
    }

    // If server has a pending task, push to worker queue
    if (resp.pending() && resp.has_task_desc()) {
      bool new_task = false;
      {
        std::lock_guard<std::mutex> lk(g_worker.mu);
        // Dedup: check if task already in queue
        bool found = false;
        for (auto& t : g_worker.queue) {
          if (t.task_id() == resp.task_desc().task_id()) {
            found = true;
            break;
          }
        }
        if (!found) {
          g_worker.queue.push_back(resp.task_desc());
          new_task = true;
        }
      }
      if (new_task) {
        g_worker.cv.notify_one();
        LOG(INFO) << "Received task " << resp.task_desc().task_id()
                  << " type=" << resp.task_desc().task_type()
                  << " profiler=" << resp.task_desc().profiler_type();
      }
    }

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

// ── Worker thread (real profiler execution) ────────────────────────
static void WorkerThread(std::shared_ptr<grpc::Channel> channel) {
  auto hm_stub = drop::hotmethod::Hotmethod::NewStub(channel);

  while (true) {
    drop::hotmethod::TaskDesc task;
    {
      std::unique_lock<std::mutex> lk(g_worker.mu);
      g_worker.cv.wait(lk, [] { return !g_worker.queue.empty() || g_worker.shutdown; });
      if (g_worker.shutdown && g_worker.queue.empty()) break;
      task = g_worker.queue.front();
      g_worker.queue.pop_front();
    }

    const std::string& tid = task.task_id();
    const std::string output_dir = "/tmp/drop_" + tid;

    LOG(INFO) << "Task " << tid << " started";

    // Create the appropriate profiler
    auto profiler = drop::CreateProfiler(task.profiler_type());
    if (!profiler) {
      // Unsupported profiler type — report failure
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
      drop::hotmethod::TaskResult result;
      result.set_task_id(tid);
      result.set_error_message("unsupported profiler_type=" + std::to_string(task.profiler_type()));
      google::protobuf::Empty empty;
      hm_stub->NotifyResult(&ctx, result, &empty);
      LOG(ERROR) << "Task " << tid << ": unknown profiler_type";
      continue;
    }

    // Run collection
    std::string error_msg;
    bool ok = profiler->record(task, output_dir, &error_msg);

    LOG(INFO) << "Task " << tid << " " << (ok ? "finished" : "failed: " + error_msg);

    // Report result back to server
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    drop::hotmethod::TaskResult result;
    result.set_task_id(tid);
    if (!ok) {
      result.set_error_message(error_msg);
    } else {
      // Upload collected files to MinIO via apiserver
      auto files = profiler->collect_result();
      std::string uploaded_cos_key;
      bool upload_ok = true;
      for (size_t i = 0; i < files.size(); ++i) {
        std::string filepath = output_dir + "/" + files[i];
        std::string this_cos_key;
        if (!UploadFileToAPI(tid, filepath, &this_cos_key)) {
          LOG(ERROR) << "Failed to upload " << filepath << ": " << this_cos_key;
          upload_ok = false;
          result.set_error_message("upload failed: " + this_cos_key);
          break;
        }
        if (i == 0) uploaded_cos_key = this_cos_key;
      }
      if (upload_ok) {
        result.set_cos_key(uploaded_cos_key);
      }
    }

    google::protobuf::Empty empty;
    auto status = hm_stub->NotifyResult(&ctx, result, &empty);
    if (!status.ok()) {
      LOG(ERROR) << "NotifyResult failed for " << tid << ": " << status.error_message();
    } else {
      LOG(INFO) << "NotifyResult OK for " << tid;
    }
  }
}

// ── Auto-detect hostname and IP ─────────────────────────────────────
static std::string DetectHostname() {
  char buf[256];
  if (gethostname(buf, sizeof(buf)) == 0) return std::string(buf);
  return "unknown";
}

static std::string DetectIP() {
  char buf[256];
  if (gethostname(buf, sizeof(buf)) == 0) {
    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    if (getaddrinfo(buf, nullptr, &hints, &res) == 0 && res) {
      char ip[INET_ADDRSTRLEN];
      auto* sa = reinterpret_cast<sockaddr_in*>(res->ai_addr);
      inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
      freeaddrinfo(res);
      return std::string(ip);
    }
  }
  return "127.0.0.1";
}

// ── main ────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  // Set log directory BEFORE daemonizing so glog writes to files, not stderr
  FLAGS_log_dir = "/tmp";
  FLAGS_logbufsecs = 0;  // Flush immediately

  // Daemonize BEFORE initializing glog, so glog gets the right file descriptors
  if (!FLAGS_foreground) {
    Daemonize();
  }

  google::InitGoogleLogging(argv[0]);

  // System constants for PidStats
  g_clk_tck = sysconf(_SC_CLK_TCK);
  g_page_size = sysconf(_SC_PAGESIZE);
  if (g_clk_tck <= 0) g_clk_tck = 100;
  if (g_page_size <= 0) g_page_size = 4096;

  // Auto-detect hostname/IP
  if (FLAGS_hostname.empty()) FLAGS_hostname = DetectHostname();
  if (FLAGS_ip.empty()) FLAGS_ip = DetectIP();
  if (FLAGS_uid.empty()) FLAGS_uid = FLAGS_ip + "-" + std::to_string(getpid());

  LOG(INFO) << "drop_agent starting";
  LOG(INFO) << "  server_addr = " << FLAGS_server_addr;
  LOG(INFO) << "  hostname    = " << FLAGS_hostname;
  LOG(INFO) << "  ip          = " << FLAGS_ip;
  LOG(INFO) << "  uid         = " << FLAGS_uid;
  LOG(INFO) << "  version     = " << FLAGS_agent_version;
  LOG(INFO) << "  foreground  = " << (FLAGS_foreground ? "true" : "false");

  if (!FLAGS_foreground) {
    LOG(INFO) << "drop_agent daemonized, pid=" << getpid();
  }

  // Install signal handlers
  signal(SIGTERM, SignalHandler);
  signal(SIGINT, SignalHandler);

  // Create gRPC channel
  auto channel = grpc::CreateChannel(
      FLAGS_server_addr, grpc::InsecureChannelCredentials());

  // Start heartbeat thread (1Hz, never blocks)
  std::thread hb_thread(HeartbeatThread, channel);
  hb_thread.detach();

  // Start worker thread
  std::thread wk_thread(WorkerThread, channel);

  // Main thread: wait for worker to finish
  wk_thread.join();

  LOG(INFO) << "drop_agent exiting";
  if (!FLAGS_foreground) {
    // Clean up PID file
    unlink("/tmp/drop_agent.pid");
  }
  return 0;
}
