#include <gtest/gtest.h>

#include <thread>
#include <chrono>
#include <cstdio>
#include <fstream>

#include "task_queue.h"
#include "profiler_factory.h"

using namespace drop;

// ══ TaskQueue Tests ════════════════════════════════════════════════════════

class TaskQueueTest : public ::testing::Test {
protected:
  TaskQueue q;
  hotmethod::TaskDesc makeTask(const std::string& tid) {
    hotmethod::TaskDesc d;
    d.set_task_id(tid);
    return d;
  }
};

TEST_F(TaskQueueTest, PushThenPopSameIP) {
  q.Push("10.0.0.1", makeTask("t1"));
  hotmethod::TaskDesc out;
  ASSERT_TRUE(q.Pop("10.0.0.1", &out));
  EXPECT_EQ(out.task_id(), "t1");
}

TEST_F(TaskQueueTest, PopReturnsFIFO) {
  q.Push("10.0.0.1", makeTask("first"));
  q.Push("10.0.0.1", makeTask("second"));
  hotmethod::TaskDesc out;
  ASSERT_TRUE(q.Pop("10.0.0.1", &out));
  EXPECT_EQ(out.task_id(), "first");
  ASSERT_TRUE(q.Pop("10.0.0.1", &out));
  EXPECT_EQ(out.task_id(), "second");
}

TEST_F(TaskQueueTest, PopEmptyReturnsFalse) {
  hotmethod::TaskDesc out;
  EXPECT_FALSE(q.Pop("10.0.0.1", &out));
}

TEST_F(TaskQueueTest, PopUnknownIPReturnsFalse) {
  q.Push("10.0.0.1", makeTask("t1"));
  hotmethod::TaskDesc out;
  EXPECT_FALSE(q.Pop("10.0.0.2", &out));
}

TEST_F(TaskQueueTest, SizeTracksDepth) {
  EXPECT_EQ(q.Size("10.0.0.1"), 0u);
  q.Push("10.0.0.1", makeTask("t1"));
  EXPECT_EQ(q.Size("10.0.0.1"), 1u);
  q.Push("10.0.0.1", makeTask("t2"));
  EXPECT_EQ(q.Size("10.0.0.1"), 2u);
}

TEST_F(TaskQueueTest, SizeZeroForUnknownIP) {
  EXPECT_EQ(q.Size("unknown"), 0u);
}

TEST_F(TaskQueueTest, DifferentIPsAreIndependent) {
  q.Push("10.0.0.1", makeTask("a"));
  q.Push("10.0.0.2", makeTask("b"));
  EXPECT_EQ(q.Size("10.0.0.1"), 1u);
  EXPECT_EQ(q.Size("10.0.0.2"), 1u);
  hotmethod::TaskDesc out;
  ASSERT_TRUE(q.Pop("10.0.0.1", &out));
  EXPECT_EQ(out.task_id(), "a");
  EXPECT_EQ(q.Size("10.0.0.1"), 0u);
  EXPECT_EQ(q.Size("10.0.0.2"), 1u);
}

TEST_F(TaskQueueTest, ConcurrentPushPop) {
  const int N = 100;
  std::thread producer([this]() {
    for (int i = 0; i < N; ++i) {
      q.Push("10.0.0.1", makeTask("t" + std::to_string(i)));
    }
  });
  int popped = 0;
  std::thread consumer([this, &popped]() {
    for (int i = 0; i < N; ++i) {
      hotmethod::TaskDesc out;
      while (!q.Pop("10.0.0.1", &out)) {
        std::this_thread::yield();
      }
      ++popped;
    }
  });
  producer.join();
  consumer.join();
  EXPECT_EQ(popped, N);
}

// ══ HeartbeatTracker Tests ════════════════════════════════════════════════

TEST(HeartbeatTrackerTest, IsOnlineAfterUpdate) {
  HeartbeatTracker ht;
  ht.Update("10.0.0.1");
  EXPECT_TRUE(ht.IsOnline("10.0.0.1", 30));
}

TEST(HeartbeatTrackerTest, IsOfflineForUnknownIP) {
  HeartbeatTracker ht;
  EXPECT_FALSE(ht.IsOnline("10.0.0.1", 30));
}

TEST(HeartbeatTrackerTest, IsOfflineAfterTimeout) {
  HeartbeatTracker ht;
  ht.Update("10.0.0.1");
  // Use 0-second timeout → immediately stale
  EXPECT_FALSE(ht.IsOnline("10.0.0.1", 0));
}

TEST(HeartbeatTrackerTest, AllAgentsMixedStatus) {
  HeartbeatTracker ht;
  ht.Update("10.0.0.1");  // recent
  ht.Update("10.0.0.2");  // recent
  auto agents = ht.AllAgents(30);
  EXPECT_EQ(agents.size(), 2u);
  for (auto& [ip, online] : agents) {
    EXPECT_TRUE(online);
  }
}

// ══ State Machine Tests ═══════════════════════════════════════════════════

TEST(StateMachineTest, PendingToRunning)   { EXPECT_TRUE(IsTransitionAllowed(0, 1)); }
TEST(StateMachineTest, PendingToFailed)    { EXPECT_TRUE(IsTransitionAllowed(0, 4)); }
TEST(StateMachineTest, RunningToUploading) { EXPECT_TRUE(IsTransitionAllowed(1, 2)); }
TEST(StateMachineTest, RunningToFailed)    { EXPECT_TRUE(IsTransitionAllowed(1, 4)); }
TEST(StateMachineTest, UploadingToDone)    { EXPECT_TRUE(IsTransitionAllowed(2, 3)); }
TEST(StateMachineTest, UploadingToFailed)  { EXPECT_TRUE(IsTransitionAllowed(2, 4)); }
TEST(StateMachineTest, FailedToPending)    { EXPECT_TRUE(IsTransitionAllowed(4, 0)); }
TEST(StateMachineTest, SameStateNotAllowed){ EXPECT_FALSE(IsTransitionAllowed(0, 0)); }
TEST(StateMachineTest, DoneToRunning)      { EXPECT_FALSE(IsTransitionAllowed(3, 1)); }
TEST(StateMachineTest, PendingToDone)      { EXPECT_FALSE(IsTransitionAllowed(0, 3)); }
TEST(StateMachineTest, RunningToDone)      { EXPECT_FALSE(IsTransitionAllowed(1, 3)); }
TEST(StateMachineTest, DoneToFailed)       { EXPECT_FALSE(IsTransitionAllowed(3, 4)); }

// ══ ProfilerFactory Tests ═════════════════════════════════════════════════

TEST(ProfilerFactoryTest, CreatePerf) {
  auto p = CreateProfiler(0);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->profiler_type(), 0);
}

TEST(ProfilerFactoryTest, CreateAsyncProfiler) {
  auto p = CreateProfiler(1);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->profiler_type(), 1);
}

TEST(ProfilerFactoryTest, CreateEbpf) {
  auto p = CreateProfiler(3);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->profiler_type(), 3);
}

TEST(ProfilerFactoryTest, UnknownTypeReturnsNull) {
  auto p = CreateProfiler(2);
  EXPECT_EQ(p, nullptr);
  p = CreateProfiler(99);
  EXPECT_EQ(p, nullptr);
}

TEST(ProfilerFactoryTest, CollectResultEmptyBeforeRecord) {
  auto p = CreateProfiler(0);
  ASSERT_NE(p, nullptr);
  EXPECT_TRUE(p->collect_result().empty());
}

TEST(ProfilerFactoryTest, AsyncProfilerRejectsPidZero) {
  auto p = CreateProfiler(1);
  ASSERT_NE(p, nullptr);
  hotmethod::TaskDesc desc;
  desc.mutable_sample_argv()->set_pid(0);
  desc.mutable_sample_argv()->set_duration(5);
  std::string err;
  EXPECT_FALSE(p->record(desc, "/tmp", &err));
  EXPECT_FALSE(err.empty());
}

TEST(ProfilerFactoryTest, AsyncProfilerRejectsNegativePid) {
  auto p = CreateProfiler(1);
  ASSERT_NE(p, nullptr);
  hotmethod::TaskDesc desc;
  desc.mutable_sample_argv()->set_pid(-1);
  desc.mutable_sample_argv()->set_duration(5);
  std::string err;
  EXPECT_FALSE(p->record(desc, "/tmp", &err));
  EXPECT_FALSE(err.empty());
}
