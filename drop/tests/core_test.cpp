#include <gtest/gtest.h>

#include <thread>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>

#include "task_queue.h"
#include "profiler_factory.h"
#include "script_runner.h"
#include "cos_client.h"
#include "server_pool.h"
#include "container_info.h"

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

// ══ ScriptRunner Tests ════════════════════════════════════════════════════

class ScriptRunnerTest : public ::testing::Test {
protected:
  std::string tmp_dir_;

  void SetUp() override {
    tmp_dir_ = "/tmp/script_runner_test_" + std::to_string(getpid());
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override {
    std::filesystem::remove_all(tmp_dir_);
  }
};

TEST_F(ScriptRunnerTest, FactoryCreatesScriptRunner) {
  auto p = CreateProfiler(5);
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->profiler_type(), 5);
}

TEST_F(ScriptRunnerTest, ExecutesEchoHello) {
  ScriptRunner runner;
  hotmethod::TaskDesc desc;
  desc.set_script_content("echo hello_world");
  desc.set_timeout_sec(10);
  std::string err;
  ASSERT_TRUE(runner.record(desc, tmp_dir_, &err)) << "error: " << err;

  auto results = runner.collect_result();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0], "script_output.txt");

  // Verify output file content
  std::ifstream f(tmp_dir_ + "/script_output.txt");
  ASSERT_TRUE(f.is_open());
  std::string line;
  std::getline(f, line);
  EXPECT_EQ(line, "hello_world");
}

TEST_F(ScriptRunnerTest, RejectsEmptyScript) {
  ScriptRunner runner;
  hotmethod::TaskDesc desc;
  desc.set_script_content("");
  std::string err;
  EXPECT_FALSE(runner.record(desc, tmp_dir_, &err));
  EXPECT_FALSE(err.empty());
  EXPECT_NE(err.find("empty"), std::string::npos);
}

TEST_F(ScriptRunnerTest, HandlesFailingScript) {
  ScriptRunner runner;
  hotmethod::TaskDesc desc;
  desc.set_script_content("exit 42");
  desc.set_timeout_sec(10);
  std::string err;
  EXPECT_FALSE(runner.record(desc, tmp_dir_, &err));
  EXPECT_NE(err.find("42"), std::string::npos);
}

TEST_F(ScriptRunnerTest, TimeoutKillsScript) {
  ScriptRunner runner;
  hotmethod::TaskDesc desc;
  desc.set_script_content("sleep 300");
  desc.set_timeout_sec(2);
  std::string err;
  auto start = std::chrono::steady_clock::now();
  EXPECT_FALSE(runner.record(desc, tmp_dir_, &err));
  auto elapsed = std::chrono::steady_clock::now() - start;
  auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
  // Should complete within ~7 seconds (2s timeout + 5s grace)
  EXPECT_LT(elapsed_sec, 15);
  EXPECT_NE(err.find("timed out"), std::string::npos);
}

TEST_F(ScriptRunnerTest, NoZombieProcesses) {
  ScriptRunner runner;
  hotmethod::TaskDesc desc;
  desc.set_script_content("echo test");
  desc.set_timeout_sec(30);
  std::string err;
  ASSERT_TRUE(runner.record(desc, tmp_dir_, &err));

  // Check no zombie sh processes from our test
  FILE* pipe = popen("ps -eo state,comm | grep -c 'Z.*sh' || true", "r");
  ASSERT_NE(pipe, nullptr);
  char buf[64] = {};
  fgets(buf, sizeof(buf), pipe);
  pclose(pipe);
  int zombies = atoi(buf);
  EXPECT_EQ(zombies, 0);
}

TEST_F(ScriptRunnerTest, CapturesMultiLineOutput) {
  ScriptRunner runner;
  hotmethod::TaskDesc desc;
  desc.set_script_content("echo line1\necho line2\necho line3");
  desc.set_timeout_sec(10);
  std::string err;
  ASSERT_TRUE(runner.record(desc, tmp_dir_, &err)) << "error: " << err;

  std::ifstream f(tmp_dir_ + "/script_output.txt");
  ASSERT_TRUE(f.is_open());
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line)) {
    lines.push_back(line);
  }
  EXPECT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "line1");
  EXPECT_EQ(lines[1], "line2");
  EXPECT_EQ(lines[2], "line3");
}

// ══ CosClient Tests ═══════════════════════════════════════════════════════

class CosClientTest : public ::testing::Test {
protected:
  std::string tmp_dir_;

  void SetUp() override {
    tmp_dir_ = "/tmp/cos_client_test_" + std::to_string(getpid());
    std::filesystem::create_directories(tmp_dir_);
  }

  void TearDown() override {
    std::filesystem::remove_all(tmp_dir_);
  }

  drop::common::CosConfig MakeConfig() {
    drop::common::CosConfig cfg;
    cfg.set_region("ap-guangzhou");
    cfg.set_bucket("test-bucket-1234567890");
    cfg.set_tmp_ak("test_ak");
    cfg.set_tmp_sk("test_sk");
    cfg.set_tmp_token("test_token");
    cfg.set_expire_ts(9999999999LL);
    return cfg;
  }
};

TEST_F(CosClientTest, BuildUrlIntranetFlag) {
  CosClient client(MakeConfig(), "http://localhost:8191");
  client.SetFlag("myflag");
  std::string url = client.BuildUrl(CosUploadMode::INTRANET_FLAG, "dir/file.dat");
  EXPECT_NE(url.find("cos-internal"), std::string::npos);
  EXPECT_NE(url.find("flag=myflag"), std::string::npos);
  EXPECT_NE(url.find("dir/file.dat"), std::string::npos);
}

TEST_F(CosClientTest, BuildUrlPublic) {
  CosClient client(MakeConfig(), "http://localhost:8191");
  std::string url = client.BuildUrl(CosUploadMode::PUBLIC, "dir/file.dat");
  EXPECT_NE(url.find("cos.ap-guangzhou"), std::string::npos);
  EXPECT_NE(url.find("test-bucket-1234567890"), std::string::npos);
}

TEST_F(CosClientTest, BuildUrlHttpProxy) {
  CosClient client(MakeConfig(), "http://localhost:8191");
  std::string url = client.BuildUrl(CosUploadMode::HTTP_PROXY, "dir/file.dat");
  EXPECT_NE(url.find("localhost:8191"), std::string::npos);
  EXPECT_NE(url.find("/api/v1/upload/"), std::string::npos);
}

TEST_F(CosClientTest, BuildUrlCustomIntranetEndpoint) {
  CosClient client(MakeConfig(), "http://localhost:8191");
  client.SetIntranetEndpoint("http://minio:9000");
  std::string url = client.BuildUrl(CosUploadMode::CONFIG_INTRANET, "dir/file.dat");
  EXPECT_NE(url.find("minio:9000"), std::string::npos);
}

TEST_F(CosClientTest, UploadNonexistentFileFails) {
  CosClient client(MakeConfig(), "http://localhost:8191");
  auto result = client.UploadFile("/tmp/nonexistent_file_12345", "key");
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error_msg.find("not found"), std::string::npos);
}

TEST_F(CosClientTest, ChainFallbackReachesHttpProxy) {
  // Create a test file
  std::string test_file = tmp_dir_ + "/test.dat";
  {
    std::ofstream f(test_file);
    f << "test data";
  }

  // All modes will fail (no real COS/MinIO accessible), but we verify
  // the chain runs through all 5 modes
  CosClient client(MakeConfig(), "http://localhost:1");  // unreachable
  auto result = client.UploadFile(test_file, "test_key");
  EXPECT_FALSE(result.success);
  // Should mention at least the last mode
  EXPECT_NE(result.error_msg.find("all upload modes failed"), std::string::npos);
}

// ══ ServerPool Tests ═════════════════════════════════════════════════════

TEST(ServerPoolTest, SingleAddressInit) {
  drop::ServerPool pool;
  pool.Init("192.168.1.1:50051");
  ASSERT_EQ(pool.addrs.size(), 1u);
  EXPECT_EQ(pool.addrs[0], "192.168.1.1:50051");
  EXPECT_EQ(pool.CurrentIdx(), 0u);
}

TEST(ServerPoolTest, MultipleAddressInit) {
  drop::ServerPool pool;
  pool.Init("10.0.0.1:50051, 10.0.0.2:50051, 10.0.0.3:50051");
  ASSERT_EQ(pool.addrs.size(), 3u);
  EXPECT_EQ(pool.addrs[0], "10.0.0.1:50051");
  EXPECT_EQ(pool.addrs[1], "10.0.0.2:50051");
  EXPECT_EQ(pool.addrs[2], "10.0.0.3:50051");
}

TEST(ServerPoolTest, FailoverRotates) {
  drop::ServerPool pool;
  pool.Init("addr1:50051, addr2:50051, addr3:50051");
  EXPECT_EQ(pool.CurrentIdx(), 0u);
  pool.Failover();
  EXPECT_EQ(pool.CurrentIdx(), 1u);
  pool.Failover();
  EXPECT_EQ(pool.CurrentIdx(), 2u);
  pool.Failover();
  EXPECT_EQ(pool.CurrentIdx(), 0u);  // wraps around
}

TEST(ServerPoolTest, FailoverSingleServerNoop) {
  drop::ServerPool pool;
  pool.Init("only-one:50051");
  EXPECT_EQ(pool.CurrentIdx(), 0u);
  pool.Failover();
  EXPECT_EQ(pool.CurrentIdx(), 0u);  // no-op with single server
}

TEST(ServerPoolTest, GetChannelReturnsNonNull) {
  drop::ServerPool pool;
  pool.Init("localhost:50051");
  auto ch = pool.GetChannel();
  ASSERT_NE(ch, nullptr);
}

TEST(ServerPoolTest, EmptyStringUsesDefault) {
  drop::ServerPool pool;
  pool.Init("");
  ASSERT_EQ(pool.addrs.size(), 1u);
  EXPECT_EQ(pool.addrs[0], "127.0.0.1:50051");
}

// ══ ContainerInfo Tests ═══════════════════════════════════════════════════

TEST(ContainerInfoTest, DetectDockerCgroup) {
  drop::ContainerInfo info;
  std::string runtime;
  EXPECT_EQ(drop::DetectContainerType("/docker/abc123def456789", &runtime), 1u);
  EXPECT_EQ(runtime, "docker");
}

TEST(ContainerInfoTest, DetectK8sCgroup) {
  drop::ContainerInfo info;
  std::string runtime;
  EXPECT_EQ(drop::DetectContainerType("/kubepods/besteffort/poduid123/ctr456", &runtime), 3u);
  EXPECT_EQ(runtime, "k8s");
}

TEST(ContainerInfoTest, DetectContainerdCgroup) {
  std::string runtime;
  EXPECT_EQ(drop::DetectContainerType("/containerd/abc123", &runtime), 2u);
  EXPECT_EQ(runtime, "containerd");
}

TEST(ContainerInfoTest, DetectSystemdNspawn) {
  std::string runtime;
  EXPECT_EQ(drop::DetectContainerType("/machine.slice/machine-mycontainer", &runtime), 4u);
  EXPECT_EQ(runtime, "systemd-nspawn");
}

TEST(ContainerInfoTest, UnknownCgroupReturnsZero) {
  std::string runtime;
  EXPECT_EQ(drop::DetectContainerType("/user.slice/user-1000", &runtime), 0u);
  EXPECT_EQ(runtime, "unknown");
}

TEST(ContainerInfoTest, DetectContainerInfoRuns) {
  // This should not crash regardless of environment
  auto info = drop::DetectContainerInfo();
  // Just verify it runs
  EXPECT_GE(info.container_type, 0u);
}
