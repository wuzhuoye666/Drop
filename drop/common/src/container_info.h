#pragma once

#include <string>
#include <cstdint>

namespace drop {

// Container environment information detected from cgroup and mountinfo.
struct ContainerInfo {
  bool is_container = false;
  uint32_t container_type = 0;  // 0=unknown, 1=docker, 2=containerd, 3=k8s, 4=systemd-nspawn
  std::string container_id;     // short container ID from cgroup
  std::string runtime;          // "docker", "containerd", "k8s", etc.
  std::string cgroup_path;      // raw cgroup path
};

// Detect container info from /proc/self/cgroup and /proc/self/mountinfo.
ContainerInfo DetectContainerInfo();

// Parse /proc/self/cgroup to extract container runtime and ID.
// Returns true if a container environment is detected.
bool ParseCgroup(ContainerInfo* info);

// Detect container type from cgroup path patterns.
// E.g. "/docker/<id>", "/kubepods/besteffort/pod<uid>/<id>", "/containerd/<id>"
uint32_t DetectContainerType(const std::string& cgroup_path, std::string* runtime);

}  // namespace drop
