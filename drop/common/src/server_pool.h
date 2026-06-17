#pragma once

#include <grpcpp/grpcpp.h>

#include <string>
#include <vector>
#include <mutex>
#include <sstream>
#include <glog/logging.h>

namespace drop {

// Multi-server failover pool for gRPC channels.
// Supports comma-separated server addresses and automatic failover
// after consecutive failures exceed a threshold.
struct ServerPool {
  std::vector<std::string> addrs;           // all server addresses
  size_t current_idx = 0;                   // index of active server
  std::shared_ptr<grpc::Channel> channel;   // current active channel
  std::mutex mu;

  // Parse comma-separated server addresses and connect to the first one
  void Init(const std::string& addr_list) {
    std::istringstream ss(addr_list);
    std::string addr;
    while (std::getline(ss, addr, ',')) {
      // Trim whitespace
      while (!addr.empty() && addr.front() == ' ') addr.erase(0, 1);
      while (!addr.empty() && addr.back() == ' ')  addr.pop_back();
      if (!addr.empty()) addrs.push_back(addr);
    }
    if (addrs.empty()) addrs.push_back("127.0.0.1:50051");
    Connect(0);
  }

  void Connect(size_t idx) {
    current_idx = idx % addrs.size();
    channel = grpc::CreateChannel(
        addrs[current_idx], grpc::InsecureChannelCredentials());
    LOG(INFO) << "ServerPool: connected to [" << current_idx
              << "] " << addrs[current_idx];
  }

  // Called when current server fails; rotates to next server
  void Failover() {
    std::lock_guard<std::mutex> lk(mu);
    size_t next = (current_idx + 1) % addrs.size();
    if (next != current_idx) {
      LOG(WARNING) << "ServerPool: failover from [" << current_idx
                   << "] " << addrs[current_idx] << " to ["
                   << next << "] " << addrs[next];
    }
    Connect(next);
  }

  std::shared_ptr<grpc::Channel> GetChannel() {
    std::lock_guard<std::mutex> lk(mu);
    return channel;
  }

  std::string CurrentAddr() {
    std::lock_guard<std::mutex> lk(mu);
    return addrs[current_idx];
  }

  size_t CurrentIdx() {
    std::lock_guard<std::mutex> lk(mu);
    return current_idx;
  }
};

}  // namespace drop
