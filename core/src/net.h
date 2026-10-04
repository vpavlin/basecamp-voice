#pragma once

#include <functional>
#include <string>
#include <vector>

// HTTP through libcurl: verified, resumable downloads and plain requests.
namespace net {

// Called with (received, total) as bytes arrive; return false to stop.
using Progress = std::function<bool(long long received, long long total)>;

// Downloads url into dest. The partial file (dest + ".part") survives a stop
// and is resumed next time. The finished file must have exactly `size` bytes
// (when size > 0) and the given sha256 (lowercase hex), or it is deleted.
bool download(const std::string& url, const std::string& dest, long long size,
              const std::string& sha256, const Progress& progress, std::string* error);

// A request with a body (empty for GET). Fills status and response.
// `abort`, when given, is polled during the transfer; returning true stops it.
bool request(const std::string& method, const std::string& url, const std::string& body,
             const std::vector<std::string>& headers, int timeoutSec,
             long* status, std::string* response, std::string* error,
             const std::function<bool()>& abort = {});

// Lowercase hex sha256 of a file; empty on error.
std::string sha256File(const std::string& path, std::string* error);

}  // namespace net
