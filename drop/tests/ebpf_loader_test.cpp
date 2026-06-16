#include <gtest/gtest.h>

#include "ebpf_loader.h"
#include "ebpf_profiler.h"
#include "profiler_factory.h"

#include <fstream>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <sys/wait.h>
#include <csignal>

// Path to the pre-compiled eBPF object
static const char* kBpfObjectPath = "/root/Drop/drop/common/bpf/offcpu.bpf.o";

// ── EbpfLoader: load + unload ────────────────────────────────────────
TEST(EbpfLoaderTest, LoadAndUnload) {
  drop::EbpfLoader loader;
  std::string err;
  ASSERT_TRUE(loader.load(kBpfObjectPath, &err)) << err;
  loader.unload();
  // Should not crash on double-unload
  loader.unload();
}

// ── EbpfLoader: full lifecycle with valid output ─────────────────────
TEST(EbpfLoaderTest, LifecycleProducesOutput) {
  drop::EbpfLoader loader;
  std::string err;
  ASSERT_TRUE(loader.load(kBpfObjectPath, &err)) << err;
  ASSERT_TRUE(loader.attach(0, &err)) << err;

  std::string out = "/tmp/ebpf_test_lifecycle.txt";
  unlink(out.c_str());
  ASSERT_TRUE(loader.poll(2, out, &err)) << err;

  loader.detach();
  loader.unload();

  // Verify output file
  ASSERT_TRUE(std::filesystem::exists(out));
  auto sz = std::filesystem::file_size(out);
  EXPECT_GT(sz, 0u) << "Output file is empty";

  // Verify folded stack format (last token on each line is a number)
  std::ifstream f(out);
  int valid_lines = 0;
  std::string line;
  while (std::getline(f, line)) {
    auto last_space = line.rfind(' ');
    if (last_space != std::string::npos) {
      std::string count_str = line.substr(last_space + 1);
      try {
        int count = std::stoi(count_str);
        if (count > 0) valid_lines++;
      } catch (...) {}
    }
  }
  EXPECT_GT(valid_lines, 0) << "No valid folded-stack lines found";

  unlink(out.c_str());
}

// ── EbpfLoader: map cleanup — second poll round works ────────────────
TEST(EbpfLoaderTest, MapCleanupAllowsRepeatedPolls) {
  drop::EbpfLoader loader;
  std::string err;
  loader.load(kBpfObjectPath, &err);
  loader.attach(0, &err);

  std::string out1 = "/tmp/ebpf_test_round1.txt";
  std::string out2 = "/tmp/ebpf_test_round2.txt";
  unlink(out1.c_str()); unlink(out2.c_str());

  loader.poll(2, out1, &err);
  loader.poll(2, out2, &err);

  loader.detach();
  loader.unload();

  auto sz1 = std::filesystem::file_size(out1);
  auto sz2 = std::filesystem::file_size(out2);
  EXPECT_GT(sz1, 0u) << "Round 1 empty";
  EXPECT_GT(sz2, 0u) << "Round 2 empty (map exhaustion bug?)";

  unlink(out1.c_str()); unlink(out2.c_str());
}

// ── EbpfLoader: PID filter correctness ───────────────────────────────
TEST(EbpfLoaderTest, PidFilterWorks) {
  // Fork a child that sleeps, then filter for it
  pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    while (true) sleep(1);
    _exit(0);
  }
  sleep(1); // let child start and go off-CPU

  drop::EbpfLoader loader;
  std::string err;
  loader.load(kBpfObjectPath, &err);
  loader.attach(child, &err);

  std::string out = "/tmp/ebpf_test_pid.txt";
  unlink(out.c_str());
  loader.poll(3, out, &err);

  kill(child, SIGKILL);
  waitpid(child, nullptr, 0);

  loader.detach();
  loader.unload();

  // The output should contain __schedule since the child was sleeping
  // For PID!=0, the BPF program filters so only that process stacks appear
  ASSERT_TRUE(std::filesystem::exists(out));
  auto sz = std::filesystem::file_size(out);
  EXPECT_GT(sz, 0u) << "PID-filtered output empty — filter may be broken";

  unlink(out.c_str());
}

// ── EbpfLoader: load invalid path fails gracefully ───────────────────
TEST(EbpfLoaderTest, LoadInvalidPathFails) {
  drop::EbpfLoader loader;
  std::string err;
  EXPECT_FALSE(loader.load("/nonexistent/bpf.o", &err));
  EXPECT_FALSE(err.empty());
}

// ── ProfilerFactory: creates EbpfProfiler for type 3 ─────────────────
TEST(ProfilerFactoryTest, CreatesEbpfProfiler) {
  auto profiler = drop::CreateProfiler(3);
  ASSERT_NE(profiler, nullptr);
  EXPECT_EQ(profiler->profiler_type(), 3u);
}

// ── ProfilerFactory: returns nullptr for unknown types ────────────────
TEST(ProfilerFactoryTest, UnknownTypeReturnsNull) {
  auto profiler = drop::CreateProfiler(99);
  EXPECT_EQ(profiler, nullptr);
}

// ── ResolveKsym: __schedule should be resolvable ─────────────────────
// This is technically testing a static function indirectly — we verify
// the kallsyms reading works by checking that the loader produces stacks
// containing __schedule (which is always in the kernel).
TEST(EbpfLoaderIntegrationTest, KernelSymbolsResolve) {
  drop::EbpfLoader loader;
  std::string err;
  loader.load(kBpfObjectPath, &err);
  loader.attach(0, &err);

  std::string out = "/tmp/ebpf_test_ksym.txt";
  unlink(out.c_str());
  loader.poll(2, out, &err);

  loader.detach();
  loader.unload();

  // Read output and verify __schedule appears
  std::ifstream f(out);
  bool found_schedule = false;
  std::string line;
  while (std::getline(f, line)) {
    if (line.find("__schedule") != std::string::npos) {
      found_schedule = true;
      break;
    }
  }
  EXPECT_TRUE(found_schedule) << "__schedule not found in kernel stack traces";

  unlink(out.c_str());
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
