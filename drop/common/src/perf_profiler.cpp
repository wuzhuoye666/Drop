#include "profiler.h"
#include "perf_profiler.h"

#include <glog/logging.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace drop {

// Run perf record as a child process with timeout handling.
// Returns true if perf exited with code 0 and produced a non-empty output file.
bool PerfProfiler::record(const drop::hotmethod::TaskDesc& task,
                           const std::string& output_dir,
                           std::string* error_msg) {
  const auto& argv = task.sample_argv();
  int hz = argv.hz();
  if (hz <= 0) hz = 99;
  int duration = argv.duration();
  if (duration <= 0) duration = 10;
  int pid = argv.pid();
  std::string callgraph = argv.callgraph();
  if (callgraph.empty()) callgraph = "fp";
  std::string event = argv.event();
  if (event.empty()) event = "cpu-cycles";

  // Create output directory
  std::filesystem::create_directories(output_dir);

  std::string perf_data_path = output_dir + "/perf.data";
  int timeout_sec = task.timeout_sec();
  if (timeout_sec <= 0) timeout_sec = duration + 30;

  // Try with -p <pid> first, fallback to system-wide if PID is unreachable
  bool ok = false;
  if (pid > 0) {
    ok = runPerf(hz, callgraph, event, pid, duration, perf_data_path,
                 output_dir, timeout_sec, error_msg);
    if (!ok && error_msg->find("No such process") != std::string::npos) {
      LOG(WARNING) << "PID " << pid
                   << " unreachable (container PID namespace?), falling back to system-wide perf";
      ok = runPerf(hz, callgraph, event, /*pid=*/0, duration, perf_data_path,
                   output_dir, timeout_sec, error_msg);
    }
  } else {
    ok = runPerf(hz, callgraph, event, /*pid=*/0, duration, perf_data_path,
                 output_dir, timeout_sec, error_msg);
  }

  if (ok) {
    collected_files_ = {"perf.data"};
    LOG(INFO) << "PerfProfiler: collected " << perf_data_path
              << " size=" << std::filesystem::file_size(perf_data_path);
  }
  return ok;
}

bool PerfProfiler::runPerf(int hz, const std::string& callgraph,
                            const std::string& event, int pid, int duration,
                            const std::string& perf_data_path,
                            const std::string& output_dir, int timeout_sec,
                            std::string* error_msg) {
  std::string hz_str = std::to_string(hz);
  std::string dur_str = std::to_string(duration);

  pid_t child = fork();
  if (child < 0) {
    *error_msg = "fork() failed: " + std::string(strerror(errno));
    return false;
  }

  if (child == 0) {
    // Child: create new process group so we can kill the whole tree later
    setpgid(0, 0);

    // Redirect stdin/stdout to /dev/null, stderr to a log file
    std::string stderr_path = output_dir + "/perf.stderr.log";
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
      close(devnull);
    }
    int stderr_fd = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (stderr_fd >= 0) {
      dup2(stderr_fd, STDERR_FILENO);
      close(stderr_fd);
    }

    // Build argument list
    std::vector<std::string> arg_strings;
    arg_strings.push_back("perf");
    arg_strings.push_back("record");
    arg_strings.push_back("-F");
    arg_strings.push_back(hz_str);
    arg_strings.push_back("--call-graph");
    arg_strings.push_back(callgraph);
    arg_strings.push_back("-e");
    arg_strings.push_back(event);
    if (pid > 0) {
      arg_strings.push_back("-p");
      arg_strings.push_back(std::to_string(pid));
    }
    arg_strings.push_back("-o");
    arg_strings.push_back(perf_data_path);
    arg_strings.push_back("--");
    arg_strings.push_back("sleep");
    arg_strings.push_back(dur_str);

    std::vector<char*> args;
    for (auto& s : arg_strings) {
      args.push_back(s.data());
    }
    args.push_back(nullptr);

    execvp("perf", args.data());

    const char* err = "execvp(perf) failed\n";
    write(STDERR_FILENO, err, strlen(err));
    _exit(127);
  }

  // Parent: watch the child with timeout
  pid_t pgid = child;
  auto start = std::chrono::steady_clock::now();

  bool timed_out = false;
  std::mutex done_mu;
  std::condition_variable done_cv;
  bool child_done = false;

  std::thread watchdog([&]() {
    auto deadline = start + std::chrono::seconds(timeout_sec);
    {
      std::unique_lock<std::mutex> lk(done_mu);
      if (!done_cv.wait_until(lk, deadline, [&] { return child_done; })) {
        timed_out = true;
        LOG(WARNING) << "Perf timeout (" << timeout_sec
                     << "s), sending SIGTERM to pgid " << pgid;
        killpg(pgid, SIGTERM);

        auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        done_cv.wait_until(lk, kill_deadline, [&] { return child_done; });

        if (!child_done) {
          LOG(WARNING) << "Perf still alive after SIGTERM, sending SIGKILL to pgid " << pgid;
          killpg(pgid, SIGKILL);
        }
      }
    }
  });

  int wstatus = 0;
  pid_t wp = waitpid(child, &wstatus, 0);
  if (wp < 0) {
    *error_msg = "waitpid failed: " + std::string(strerror(errno));
  }

  {
    std::lock_guard<std::mutex> lk(done_mu);
    child_done = true;
  }
  done_cv.notify_all();
  watchdog.join();

  if (timed_out) {
    while (waitpid(-pgid, nullptr, WNOHANG) > 0) {}
    *error_msg = "collection timed out after " + std::to_string(timeout_sec) + "s";
    return false;
  }

  if (wp < 0) {
    return false;
  }

  if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
    // Read stderr for details
    std::string stderr_content;
    std::string stderr_path = output_dir + "/perf.stderr.log";
    std::ifstream f(stderr_path);
    if (f.is_open()) {
      std::ostringstream ss;
      ss << f.rdbuf();
      stderr_content = ss.str();
    }
    if (!stderr_content.empty()) {
      auto last_nl = stderr_content.rfind('\n', stderr_content.size() - 2);
      if (last_nl != std::string::npos) {
        stderr_content = stderr_content.substr(last_nl + 1);
      }
      while (!stderr_content.empty() && stderr_content.back() == '\n') {
        stderr_content.pop_back();
      }
    }
    *error_msg = "perf record exited with code " + std::to_string(WEXITSTATUS(wstatus));
    if (!stderr_content.empty()) {
      *error_msg += ": " + stderr_content;
    }
    return false;
  }

  if (WIFSIGNALED(wstatus)) {
    *error_msg = "perf record killed by signal " + std::to_string(WTERMSIG(wstatus));
    return false;
  }

  if (!std::filesystem::exists(perf_data_path)) {
    *error_msg = "perf.data not found after collection";
    return false;
  }
  if (std::filesystem::file_size(perf_data_path) == 0) {
    *error_msg = "perf.data is empty";
    return false;
  }

  return true;
}

std::vector<std::string> PerfProfiler::collect_result() const {
  return collected_files_;
}

uint32_t PerfProfiler::profiler_type() const {
  return 0;  // perf
}

}  // namespace drop
