#pragma once

#include <string>
#include <vector>
#include "common.pb.h"

namespace drop {

// COS upload mode for chain fallback.
// Mode 1: Intranet endpoint + FLAG marker (internal network with flag)
// Mode 2: Public internet endpoint
// Mode 3: FLAG intranet only (no config endpoint)
// Mode 4: Configured intranet only (no FLAG)
// Mode 5: HTTP proxy via apiserver (last resort)
enum class CosUploadMode : int {
  INTRANET_FLAG   = 1,
  PUBLIC          = 2,
  FLAG_INTRANET   = 3,
  CONFIG_INTRANET = 4,
  HTTP_PROXY      = 5,
};

// Result of an upload attempt.
struct CosUploadResult {
  bool success = false;
  CosUploadMode used_mode = CosUploadMode::HTTP_PROXY;
  std::string cos_key;      // object key in bucket
  std::string error_msg;    // error details if failed
};

// COS client with 5-mode chain fallback.
// Attempts each upload mode in order; if mode N fails, tries mode N+1.
// Mode 5 (HTTP proxy) always falls back to the apiserver upload endpoint.
class CosClient {
public:
  // Construct with COS config (temporary STS credentials) and
  // the apiserver address (used for HTTP proxy mode).
  CosClient(const drop::common::CosConfig& cos_config,
            const std::string& apiserver_addr);

  // Upload a file using chain fallback.
  // On success, result.success=true and result.cos_key is set.
  // On total failure, result.success=false and result.error_msg has details.
  CosUploadResult UploadFile(const std::string& local_path,
                              const std::string& object_key);

  // Set a custom endpoint override (e.g., MinIO for dev).
  // If non-empty, used as the base URL for intranet modes.
  void SetIntranetEndpoint(const std::string& endpoint);

  // Set a FLAG identifier for intranet+FLAG mode.
  void SetFlag(const std::string& flag);

  // Set HTTP proxy URL for mode 5.
  void SetHttpProxy(const std::string& proxy_url);

  // Build the URL for a given mode (public for testing).
  std::string BuildUrl(CosUploadMode mode, const std::string& object_key);

private:

  // Upload via curl PUT with COS-style auth headers.
  bool CurlPut(const std::string& url,
               const std::string& local_path,
               const std::string& auth_header,
               const std::string& token_header,
               std::string* error_msg);

  // Upload via HTTP proxy (curl multipart POST to apiserver).
  bool CurlProxyUpload(const std::string& local_path,
                       const std::string& object_key,
                       std::string* error_msg);

  drop::common::CosConfig cos_config_;
  std::string apiserver_addr_;
  std::string intranet_endpoint_;
  std::string flag_;
  std::string http_proxy_;
};

}  // namespace drop
