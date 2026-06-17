#include "cos_client.h"

#include <glog/logging.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>

namespace drop {

CosClient::CosClient(const drop::common::CosConfig& cos_config,
                     const std::string& apiserver_addr)
    : cos_config_(cos_config), apiserver_addr_(apiserver_addr) {}

void CosClient::SetIntranetEndpoint(const std::string& endpoint) {
  intranet_endpoint_ = endpoint;
}

void CosClient::SetFlag(const std::string& flag) {
  flag_ = flag;
}

void CosClient::SetHttpProxy(const std::string& proxy_url) {
  http_proxy_ = proxy_url;
}

std::string CosClient::BuildUrl(CosUploadMode mode,
                                 const std::string& object_key) {
  const std::string& bucket = cos_config_.bucket();
  const std::string& region = cos_config_.region();

  switch (mode) {
    case CosUploadMode::INTRANET_FLAG: {
      // Internal COS endpoint with FLAG suffix
      // Format: https://<bucket>-<appid>.cos-internal.<region>.myqcloud.com/<key>
      // FLAG is appended as a query param for routing
      std::string base = intranet_endpoint_.empty()
          ? "https://" + bucket + ".cos-internal." + region + ".myqcloud.com"
          : intranet_endpoint_;
      return base + "/" + object_key + (flag_.empty() ? "" : "?flag=" + flag_);
    }
    case CosUploadMode::PUBLIC: {
      // Public COS endpoint
      return "https://" + bucket + ".cos." + region + ".myqcloud.com/" + object_key;
    }
    case CosUploadMode::FLAG_INTRANET: {
      // FLAG-only intranet (no config-based endpoint)
      std::string base = intranet_endpoint_.empty()
          ? "https://" + bucket + ".cos-internal." + region + ".myqcloud.com"
          : intranet_endpoint_;
      return base + "/" + object_key + (flag_.empty() ? "" : "?flag=" + flag_);
    }
    case CosUploadMode::CONFIG_INTRANET: {
      // Configured intranet endpoint only (no FLAG)
      std::string base = intranet_endpoint_.empty()
          ? "https://" + bucket + ".cos-internal." + region + ".myqcloud.com"
          : intranet_endpoint_;
      return base + "/" + object_key;
    }
    case CosUploadMode::HTTP_PROXY: {
      // HTTP proxy mode uses apiserver upload endpoint
      return apiserver_addr_ + "/api/v1/upload/" + object_key;
    }
  }
  return "";
}

CosUploadResult CosClient::UploadFile(const std::string& local_path,
                                       const std::string& object_key) {
  if (!std::filesystem::exists(local_path)) {
    return {false, CosUploadMode::HTTP_PROXY, "",
            "file not found: " + local_path};
  }

  // Chain fallback order
  static const CosUploadMode modes[] = {
    CosUploadMode::INTRANET_FLAG,
    CosUploadMode::PUBLIC,
    CosUploadMode::FLAG_INTRANET,
    CosUploadMode::CONFIG_INTRANET,
    CosUploadMode::HTTP_PROXY,
  };

  std::string last_error;
  for (auto mode : modes) {
    std::string url = BuildUrl(mode, object_key);
    if (url.empty()) continue;

    LOG(INFO) << "CosClient: trying mode " << static_cast<int>(mode)
              << " url=" << url;

    std::string error_msg;
    bool ok = false;
    if (mode == CosUploadMode::HTTP_PROXY) {
      ok = CurlProxyUpload(local_path, object_key, &error_msg);
    } else {
      // Build COS auth headers using STS temporary credentials
      std::string auth_header = "Authorization: COS " +
          cos_config_.tmp_ak() + "/" + cos_config_.tmp_sk();
      std::string token_header = "x-cos-security-token: " +
          cos_config_.tmp_token();
      ok = CurlPut(url, local_path, auth_header, token_header, &error_msg);
    }

    if (ok) {
      LOG(INFO) << "CosClient: upload succeeded via mode "
                << static_cast<int>(mode);
      return {true, mode, object_key, ""};
    }

    LOG(WARNING) << "CosClient: mode " << static_cast<int>(mode)
                 << " failed: " << error_msg;
    last_error = "mode " + std::to_string(static_cast<int>(mode)) +
                 ": " + error_msg;
  }

  return {false, CosUploadMode::HTTP_PROXY, "",
          "all upload modes failed; last: " + last_error};
}

bool CosClient::CurlPut(const std::string& url,
                         const std::string& local_path,
                         const std::string& auth_header,
                         const std::string& token_header,
                         std::string* error_msg) {
  // Use curl PUT with COS auth headers
  // curl -sf -X PUT -H "Authorization: COS ak/sk" -H "x-cos-security-token: token" -T <file> <url>
  std::string http_code_path = local_path + ".curl_code";

  pid_t child = fork();
  if (child < 0) {
    *error_msg = "fork() failed: " + std::string(strerror(errno));
    return false;
  }

  if (child == 0) {
    // Redirect stdout to code file, stderr to /dev/null
    int code_fd = open(http_code_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (code_fd >= 0) {
      dup2(code_fd, STDOUT_FILENO);
      close(code_fd);
    }
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    execlp("curl", "curl", "-sf", "-X", "PUT",
           "-w", "%{http_code}",
           "-H", auth_header.c_str(),
           "-H", token_header.c_str(),
           "-T", local_path.c_str(),
           url.c_str(),
           static_cast<char*>(nullptr));

    _exit(127);
  }

  int wstatus = 0;
  if (waitpid(child, &wstatus, 0) < 0) {
    *error_msg = "waitpid failed: " + std::string(strerror(errno));
    return false;
  }

  if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
    *error_msg = "curl exited with code " + std::to_string(WEXITSTATUS(wstatus));
    return false;
  }

  // Read HTTP status code
  std::string code;
  {
    std::ifstream f(http_code_path);
    if (f.is_open()) {
      std::getline(f, code);
    }
  }
  std::filesystem::remove(http_code_path);

  if (code.empty()) {
    *error_msg = "no HTTP code from curl";
    return false;
  }

  int http_status = std::stoi(code);
  if (http_status >= 200 && http_status < 300) {
    return true;
  }

  *error_msg = "HTTP " + code;
  return false;
}

bool CosClient::CurlProxyUpload(const std::string& local_path,
                                 const std::string& object_key,
                                 std::string* error_msg) {
  // Use curl multipart POST to apiserver (same as UploadFileToAPI)
  std::string url = apiserver_addr_ + "/api/v1/upload/" + object_key;
  std::string http_code_path = local_path + ".proxy_code";

  pid_t child = fork();
  if (child < 0) {
    *error_msg = "fork() failed: " + std::string(strerror(errno));
    return false;
  }

  if (child == 0) {
    int code_fd = open(http_code_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (code_fd >= 0) {
      dup2(code_fd, STDOUT_FILENO);
      close(code_fd);
    }
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    execlp("curl", "curl", "-sf",
           "-w", "%{http_code}",
           "-F", ("file=@" + local_path).c_str(),
           url.c_str(),
           static_cast<char*>(nullptr));

    _exit(127);
  }

  int wstatus = 0;
  if (waitpid(child, &wstatus, 0) < 0) {
    *error_msg = "waitpid failed: " + std::string(strerror(errno));
    return false;
  }

  if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0) {
    *error_msg = "curl proxy exited with code " + std::to_string(WEXITSTATUS(wstatus));
    return false;
  }

  std::string code;
  {
    std::ifstream f(http_code_path);
    if (f.is_open()) {
      std::getline(f, code);
    }
  }
  std::filesystem::remove(http_code_path);

  if (code.empty()) {
    *error_msg = "no HTTP code from proxy curl";
    return false;
  }

  int http_status = std::stoi(code);
  if (http_status >= 200 && http_status < 300) {
    return true;
  }

  *error_msg = "proxy HTTP " + code;
  return false;
}

}  // namespace drop
