#include <gtest/gtest.h>

#include "async_profiler_profiler.h"
#include "profiler_factory.h"
#include "profiler.h"

// ── ProfilerFactory: creates AsyncProfilerProfiler for type 1 ─────────
TEST(AsyncProfilerFactoryTest, CreatesAsyncProfilerProfiler) {
  auto profiler = drop::CreateProfiler(1);
  ASSERT_NE(profiler, nullptr);
  EXPECT_EQ(profiler->profiler_type(), 1u);
}

// ── ProfilerFactory: still creates PerfProfiler for type 0 ────────────
TEST(AsyncProfilerFactoryTest, PerfProfilerStillWorks) {
  auto profiler = drop::CreateProfiler(0);
  ASSERT_NE(profiler, nullptr);
  EXPECT_EQ(profiler->profiler_type(), 0u);
}

// ── ProfilerFactory: still creates EbpfProfiler for type 3 ────────────
TEST(AsyncProfilerFactoryTest, EbpfProfilerStillWorks) {
  auto profiler = drop::CreateProfiler(3);
  ASSERT_NE(profiler, nullptr);
  EXPECT_EQ(profiler->profiler_type(), 3u);
}

// ── ProfilerFactory: returns nullptr for type 2 (unimplemented) ───────
TEST(AsyncProfilerFactoryTest, Type2ReturnsNull) {
  auto profiler = drop::CreateProfiler(2);
  EXPECT_EQ(profiler, nullptr);
}

// ── AsyncProfilerProfiler: record() fails gracefully without asprof ───
// Since asprof is not installed in CI, we verify the error path.
TEST(AsyncProfilerProfilerTest, RecordFailsWithoutAsprof) {
  drop::AsyncProfilerProfiler profiler;
  drop::hotmethod::TaskDesc task;
  task.mutable_sample_argv()->set_pid(getpid());
  task.mutable_sample_argv()->set_duration(1);
  task.mutable_sample_argv()->set_hz(99);
  task.mutable_sample_argv()->set_event("cpu");

  std::string error_msg;
  bool ok = profiler.record(task, "/tmp/drop_ap_test_noprof", &error_msg);
  EXPECT_FALSE(ok);
  EXPECT_FALSE(error_msg.empty());
  // Error message should mention asprof not found
  EXPECT_NE(error_msg.find("asprof"), std::string::npos)
      << "Error msg: " << error_msg;
}

// ── AsyncProfilerProfiler: record() rejects PID=0 ─────────────────────
TEST(AsyncProfilerProfilerTest, RecordRejectsPidZero) {
  drop::AsyncProfilerProfiler profiler;
  drop::hotmethod::TaskDesc task;
  task.mutable_sample_argv()->set_pid(0);
  task.mutable_sample_argv()->set_duration(1);

  std::string error_msg;
  bool ok = profiler.record(task, "/tmp/drop_ap_test_pid0", &error_msg);
  EXPECT_FALSE(ok);
  EXPECT_NE(error_msg.find("PID"), std::string::npos);
}

// ── AsyncProfilerProfiler: record() rejects negative PID ───────────────
TEST(AsyncProfilerProfilerTest, RecordRejectsNegativePid) {
  drop::AsyncProfilerProfiler profiler;
  drop::hotmethod::TaskDesc task;
  task.mutable_sample_argv()->set_pid(-1);
  task.mutable_sample_argv()->set_duration(1);

  std::string error_msg;
  bool ok = profiler.record(task, "/tmp/drop_ap_test_negpid", &error_msg);
  EXPECT_FALSE(ok);
}

// ── AsyncProfilerProfiler: collect_result() returns empty before record ─
TEST(AsyncProfilerProfilerTest, CollectResultEmptyBeforeRecord) {
  drop::AsyncProfilerProfiler profiler;
  auto result = profiler.collect_result();
  EXPECT_TRUE(result.empty());
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
