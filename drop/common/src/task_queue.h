#pragma once

#include <string>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>

#include "hotmethod.pb.h"

namespace drop {

// Thread-safe per-IP task queue.
// CreateTask pushes a TaskDesc; HealthCheck::Do pops one when the agent calls.
class TaskQueue {
public:
  void Push(const std::string& target_ip, const drop::hotmethod::TaskDesc& desc) {
    std::lock_guard<std::mutex> lk(mu_);
    queues_[target_ip].push_back(desc);
    cv_.notify_all();
  }

  bool Pop(const std::string& target_ip, drop::hotmethod::TaskDesc* out) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = queues_.find(target_ip);
    if (it == queues_.end() || it->second.empty()) return false;
    *out = it->second.front();
    it->second.pop_front();
    return true;
  }

  size_t Size(const std::string& target_ip) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = queues_.find(target_ip);
    return it == queues_.end() ? 0 : it->second.size();
  }

private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::unordered_map<std::string, std::deque<drop::hotmethod::TaskDesc>> queues_;
};

// Tracks last heartbeat timestamp for each agent IP.
class HeartbeatTracker {
public:
  void Update(const std::string& ip) {
    std::lock_guard<std::mutex> lk(mu_);
    last_hb_[ip] = std::chrono::steady_clock::now();
  }

  // Returns true if this IP has sent a heartbeat within `timeout_sec` seconds.
  bool IsOnline(const std::string& ip, int timeout_sec = 30) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = last_hb_.find(ip);
    if (it == last_hb_.end()) return false;
    auto elapsed = std::chrono::steady_clock::now() - it->second;
    return std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < timeout_sec;
  }

  // Returns all known IPs and whether they are online.
  std::vector<std::pair<std::string, bool>> AllAgents(int timeout_sec = 30) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, bool>> result;
    for (auto& [ip, ts] : last_hb_) {
      auto elapsed = std::chrono::steady_clock::now() - ts;
      bool online = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < timeout_sec;
      result.emplace_back(ip, online);
    }
    return result;
  }

private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_hb_;
};

// Simple PG connector using libpq.
// Handles: agent_info upsert, hotmethod_task update, task_status_history insert,
// agent_audit_log insert.
class PGStore {
public:
  explicit PGStore(const std::string& conninfo);
  ~PGStore();

  bool Connected() const { return conn_ != nullptr; }

  // Upsert agent_info on heartbeat: update last_heartbeat_at, hostname, version.
  void UpsertAgent(const std::string& ip, const std::string& hostname,
                   const std::string& uid, const std::string& version);

  // Update hotmethod_task status + status_info.
  void UpdateTaskStatus(const std::string& tid, int new_status, const std::string& reason);

  // Scan agents whose last_heartbeat_at is stale, update online flag, write audit log.
  // Returns number of agents marked offline.
  int ScanAgentHeartbeats(int timeout_sec = 30);

  // Get all agents from agent_info (for StatAgent).
  struct AgentRecord {
    std::string hostname;
    std::string ip;
    bool online;
    std::string uid;
    std::string version;
  };
  std::vector<AgentRecord> ListAgents();

private:
  void* conn_;  // PGconn*
};

}  // namespace drop
