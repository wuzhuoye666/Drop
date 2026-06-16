#include "profiler.h"
#include "async_profiler_profiler.h"

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

// Locate asprof binary. Search standard locations used by Docker image and
// local development setups.
std::string AsyncProfilerProfiler::findAsprof() {
  const char* candidates[] = {
    "/opt/async-profiler/bin/asprof",
    "/opt/drop/tools/async-profiler/bin/asprof",
    "/usr/local/bin/asprof",
  };
  for (const auto& p : candidates) {
    if (std::filesystem::exists(p)) return p;
  }
  // Check alongside the running binary
  char exe[4096] = {};
  ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (len > 0) {
    exe[len] = '\0';
    std::string exe_dir = std::filesystem::path(exe).parent_path().string();
    std::string rel = exe_dir + "/../tools/async-profiler/bin/asprof";
    if (std::filesystem::exists(rel)) return std::filesystem::canonical(rel);
  }
  // Check DROP_TOOLS_DIR env
  const char* tools_dir = std::getenv("DROP_TOOLS_DIR");
  if (tools_dir) {
    std::string p = std::string(tools_dir) + "/async-profiler/bin/asprof";
    if (std::filesystem::exists(p)) return p;
  }
  return {};
}

bool AsyncProfilerProfiler::record(const drop::hotmethod::TaskDesc& task,
                                     const std::string& output_dir,
                                     std::string* error_msg) {
  const auto& argv = task.sample_argv();
  int pid = argv.pid();
  int duration = argv.duration();
  if (duration <= 0) duration = 10;
  int hz = argv.hz();
  if (hz <= 0) hz = 99;
  std::string event = argv.event();
  if (event.empty()) event = "cpu";

  if (pid <= 0) {
    *error_msg = "async-profiler requires a valid PID (pid > 0)";
    return false;
  }

  // Find asprof
  std::string asprof = findAsprof();
  if (asprof.empty()) {
    *error_msg = "asprof binary not found. Install async-profiler to /opt/async-profiler/ or set DROP_TOOLS_DIR";
    return false;
  }

  LOG(INFO) << "AsyncProfilerProfiler: asprof=" << asprof
            << " pid=" << pid << " duration=" << duration
            << " event=" << event << " hz=" << hz;

  // Create output directory
  std::filesystem::create_directories(output_dir);

  // asprof outputs collapsed stacks when given .collapsed extension
  std::string output_path = output_dir + "/async_profiler.collapsed";
  int timeout_sec = task.timeout_sec();
  if (timeout_sec <= 0) timeout_sec = duration + 30;

  // Build argument list:
  //   asprof -d <duration> -f <output>.collapsed -e <event> -i <interval_ns> <pid>
  // interval in nanoseconds = 1e9 / hz
  int interval_ns = 1000000000 / hz;

  std::vector<std::string> arg_strings = {
    "asprof",
    "-d", std::to_string(duration),
    "-f", output_path,
    "-e", event,
    "-i", std::to_string(interval_ns),
    std::to_string(pid),
  };

  std::vector<char*> args;
  for (auto& s : arg_strings) {
    args.push_back(s.data());
  }
  args.push_back(nullptr);

  pid_t child = fork();
  if (child < 0) {
    *error_msg = "fork() failed: " + std::string(strerror(errno));
    return false;
  }

  if (child == 0) {
    // Child: new process group for timeout handling
    setpgid(0, 0);

    // Redirect stdout/stderr to log files
    std::string stdout_path = output_dir + "/asprof.stdout.log";
    std::string stderr_path = output_dir + "/asprof.stderr.log";
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
    int out_fd = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd >= 0) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }
    int err_fd = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (err_fd >= 0) { dup2(err_fd, STDERR_FILENO); close(err_fd); }

    execvp(asprof.c_str(), args.data());

    const char* err = "execvp(asprof) failed\n";
    write(STDERR_FILENO, err, strlen(err));
    _exit(127);
  }

  // Parent: wait with timeout
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
        LOG(WARNING) << "Async-profiler timeout (" << timeout_sec
                     << "s), sending SIGTERM to pgid " << pgid;
        killpg(pgid, SIGTERM);

        auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        done_cv.wait_until(lk, kill_deadline, [&] { return child_done; });

        if (!child_done) {
          LOG(WARNING) << "Async-profiler still alive after SIGTERM, sending SIGKILL";
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
    *error_msg = "async-profiler timed out after " + std::to_string(timeout_sec) + "s";
    return false;
  }

  if (wp < 0) return false;

  if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
    // Read stderr for details
    std::string stderr_content;
    std::string stderr_path = output_dir + "/asprof.stderr.log";
    std::ifstream f(stderr_path);
    if (f.is_open()) {
      std::ostringstream ss;
      ss << f.rdbuf();
      stderr_content = ss.str();
    }
    // Take last non-empty line
    if (!stderr_content.empty()) {
      auto last_nl = stderr_content.rfind('\n', stderr_content.size() - 2);
      if (last_nl != std::string::npos) {
        stderr_content = stderr_content.substr(last_nl + 1);
      }
      while (!stderr_content.empty() && stderr_content.back() == '\n') {
        stderr_content.pop_back();
      }
    }
    *error_msg = "asprof exited with code " + std::to_string(WEXITSTATUS(wstatus));
    if (!stderr_content.empty()) *error_msg += ": " + stderr_content;
    return false;
  }

  if (WIFSIGNALED(wstatus)) {
    *error_msg = "asprof killed by signal " + std::to_string(WTERMSIG(wstatus));
    return false;
  }

  if (!std::filesystem::exists(output_path)) {
    *error_msg = "async_profiler.collapsed not found after collection";
    return false;
  }
  if (std::filesystem::file_size(output_path) == 0) {
    *error_msg = "async_profiler.collapsed is empty";
    return false;
  }

  collected_files_ = {"async_profiler.collapsed"};
  LOG(INFO) << "AsyncProfilerProfiler: collected " << output_path
            << " size=" << std::filesystem::file_size(output_path);
  return true;
}

std::vector<std::string> AsyncProfilerProfiler::collect_result() const {
  return collected_files_;
}

uint32_t AsyncProfilerProfiler::profiler_type() const {
  return 1;  // async-profiler
}

}  // namespace drop
