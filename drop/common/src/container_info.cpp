#include "container_info.h"

#include <glog/logging.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace drop {

bool ParseCgroup(ContainerInfo* info) {
  std::ifstream f("/proc/self/cgroup");
  if (!f.is_open()) {
    LOG(WARNING) << "Cannot open /proc/self/cgroup";
    return false;
  }

  std::string line;
  while (std::getline(f, line)) {
    // Format: hierarchy-ID:controller-list:cgroup-path
    // e.g. "0::/docker/abc123..."
    // e.g. "1:name=systemd:/kubepods/besteffort/poduid/ctr-id"
    auto first_colon = line.find(':');
    if (first_colon == std::string::npos) continue;
    auto second_colon = line.find(':', first_colon + 1);
    if (second_colon == std::string::npos) continue;

    std::string cgroup_path = line.substr(second_colon + 1);
    if (cgroup_path.empty() || cgroup_path == "/") continue;

    info->cgroup_path = cgroup_path;
    std::string runtime;
    info->container_type = DetectContainerType(cgroup_path, &runtime);
    info->runtime = runtime;

    if (info->container_type > 0) {
      info->is_container = true;

      // Extract container ID (last path component that looks like a hex ID)
      // e.g. "/docker/abc123def456" -> "abc123def456"
      // e.g. "/kubepods/besteffort/poduid123/abc456def" -> "abc456def"
      auto last_slash = cgroup_path.rfind('/');
      if (last_slash != std::string::npos && last_slash + 1 < cgroup_path.size()) {
        std::string last_comp = cgroup_path.substr(last_slash + 1);
        // Docker/containerd IDs are 64-char hex strings; use first 12 chars as short ID
        if (last_comp.size() >= 12) {
          // Check if it looks like a hex ID
          bool is_hex = std::all_of(last_comp.begin(), last_comp.begin() + 12,
              [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
          if (is_hex) {
            info->container_id = last_comp.substr(0, 12);
          }
        }
      }
      return true;
    }
  }
  return false;
}

uint32_t DetectContainerType(const std::string& cgroup_path, std::string* runtime) {
  // Docker: /docker/<64-char-hex-id>
  if (cgroup_path.find("/docker/") != std::string::npos) {
    *runtime = "docker";
    return 1;
  }

  // Kubernetes (kubepods): /kubepods/.../<container-id>
  if (cgroup_path.find("/kubepods") != std::string::npos) {
    *runtime = "k8s";
    return 3;
  }

  // Containerd: /containerd/<id>
  if (cgroup_path.find("/containerd/") != std::string::npos ||
      cgroup_path.find("/system.slice/containerd") != std::string::npos) {
    *runtime = "containerd";
    return 2;
  }

  // systemd-nspawn: /machine.slice/machine-<name>
  if (cgroup_path.find("/machine.slice/machine-") != std::string::npos) {
    *runtime = "systemd-nspawn";
    return 4;
  }

  // LXC: /lxc/<name>
  if (cgroup_path.find("/lxc/") != std::string::npos) {
    *runtime = "lxc";
    return 1;  // treat as docker-like (type 1)
  }

  *runtime = "unknown";
  return 0;
}

ContainerInfo DetectContainerInfo() {
  ContainerInfo info;

  // Also check for /.dockerenv as a secondary indicator
  std::ifstream dockerenv("/.dockerenv");
  bool has_dockerenv = dockerenv.is_open();
  dockerenv.close();

  bool from_cgroup = ParseCgroup(&info);

  if (!from_cgroup && has_dockerenv) {
    info.is_container = true;
    info.container_type = 1;
    info.runtime = "docker";
  }

  // Check for Kubernetes environment variables
  if (!info.is_container) {
    const char* k8s_service = std::getenv("KUBERNETES_SERVICE_HOST");
    if (k8s_service != nullptr) {
      info.is_container = true;
      info.container_type = 3;
      info.runtime = "k8s";
    }
  }

  LOG(INFO) << "ContainerInfo: is_container=" << info.is_container
            << " type=" << info.container_type
            << " runtime=" << info.runtime
            << " id=" << info.container_id
            << " cgroup=" << info.cgroup_path;

  return info;
}

}  // namespace drop
