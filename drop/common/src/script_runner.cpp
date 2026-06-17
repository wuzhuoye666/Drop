#include "profiler.h"
#include "script_runner.h"

#include <glog/logging.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace drop {

bool ScriptRunner::record(const drop::hotmethod::TaskDesc& task,
                           const std::string& output_dir,
                           std::string* error_msg) {
  std::string script_content = task.script_content();
  if (script_content.empty()) {
    *error_msg = "script_content is empty";
    return false;
  }

  // Create output directory
  std::filesystem::create_directories(output_dir);

  // Write script content to temp file
  std::string script_path = output_dir + "/script.sh";
  {
    std::ofstream f(script_path, std::ios::out | std::ios::trunc);
    if (!f.is_open()) {
      *error_msg = "failed to create script file: " + script_path;
      return false;
    }
    f << script_content;
    f.close();
  }
  // Make executable
  if (chmod(script_path.c_str(), 0755) != 0) {
    *error_msg = "chmod failed: " + std::string(strerror(errno));
    return false;
  }

  LOG(INFO) << "ScriptRunner: executing " << script_path;

  int timeout_sec = task.timeout_sec();
  if (timeout_sec <= 0) {
    int duration = task.sample_argv().duration();
    timeout_sec = (duration > 0 ? duration : 30) + 30;
  }

  std::string stdout_path = output_dir + "/script_output.txt";
  std::string stderr_path = output_dir + "/script_stderr.log";

  pid_t child = fork();
  if (child < 0) {
    *error_msg = "fork() failed: " + std::string(strerror(errno));
    return false;
  }

  if (child == 0) {
    // Child: create new process group so we can kill the whole tree
    setpgid(0, 0);

    // Redirect stdin to /dev/null
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      close(devnull);
    }

    // Redirect stdout to output file
    int stdout_fd = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (stdout_fd >= 0) {
      dup2(stdout_fd, STDOUT_FILENO);
      close(stdout_fd);
    }

    // Redirect stderr to log file
    int stderr_fd = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (stderr_fd >= 0) {
      dup2(stderr_fd, STDERR_FILENO);
      close(stderr_fd);
    }

    // Execute: /bin/sh script_path
    execlp("/bin/sh", "sh", script_path.c_str(), static_cast<char*>(nullptr));

    const char* err = "execlp(/bin/sh) failed\n";
    write(STDERR_FILENO, err, strlen(err));
    _exit(127);
  }

  // Parent: watch child with timeout
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
        LOG(WARNING) << "ScriptRunner timeout (" << timeout_sec
                     << "s), sending SIGTERM to pgid " << pgid;
        killpg(pgid, SIGTERM);

        auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        done_cv.wait_until(lk, kill_deadline, [&] { return child_done; });

        if (!child_done) {
          LOG(WARNING) << "ScriptRunner still alive after SIGTERM, sending SIGKILL to pgid " << pgid;
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
    // Reap any remaining children in the process group
    while (waitpid(-pgid, nullptr, WNOHANG) > 0) {}
    *error_msg = "script timed out after " + std::to_string(timeout_sec) + "s";
    return false;
  }

  if (wp < 0) {
    return false;
  }

  if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
    // Read stderr for details
    std::string stderr_content;
    std::ifstream f(stderr_path);
    if (f.is_open()) {
      std::ostringstream ss;
      ss << f.rdbuf();
      stderr_content = ss.str();
    }
    // Take last non-empty line
    if (!stderr_content.empty()) {
      while (!stderr_content.empty() && stderr_content.back() == '\n') {
        stderr_content.pop_back();
      }
      auto last_nl = stderr_content.rfind('\n');
      if (last_nl != std::string::npos) {
        stderr_content = stderr_content.substr(last_nl + 1);
      }
    }
    *error_msg = "script exited with code " + std::to_string(WEXITSTATUS(wstatus));
    if (!stderr_content.empty()) {
      *error_msg += ": " + stderr_content;
    }
    return false;
  }

  if (WIFSIGNALED(wstatus)) {
    *error_msg = "script killed by signal " + std::to_string(WTERMSIG(wstatus));
    return false;
  }

  collected_files_ = {"script_output.txt"};
  LOG(INFO) << "ScriptRunner: completed successfully, output at " << stdout_path;
  return true;
}

std::vector<std::string> ScriptRunner::collect_result() const {
  return collected_files_;
}

uint32_t ScriptRunner::profiler_type() const {
  return 5;  // script
}

}  // namespace drop
