#include "net.h"

#include <cstdio>
#include <mutex>
#include <sys/stat.h>

#include <curl/curl.h>
#include <openssl/evp.h>

namespace net {
namespace {

void globalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

long long fileSize(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : -1;
}

// libcurl here is built by Nix and its default CA path does not exist on a
// user's machine: point it at the system's bundle.
void useSystemCertificates(CURL* c) {
    static const char* bundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",   // Debian, Ubuntu, Arch
        "/etc/pki/tls/certs/ca-bundle.crt",     // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",               // openSUSE
        "/etc/ssl/cert.pem",                    // Alpine, macOS
        "/etc/pki/tls/cacert.pem",
    };
    for (const char* b : bundles) {
        if (fileSize(b) > 0) {
            curl_easy_setopt(c, CURLOPT_CAINFO, b);
            break;
        }
    }
    if (fileSize("/etc/ssl/certs") >= 0) curl_easy_setopt(c, CURLOPT_CAPATH, "/etc/ssl/certs");
}

size_t writeFile(char* data, size_t size, size_t n, void* user) {
    return std::fwrite(data, size, n, static_cast<FILE*>(user)) * size;
}

size_t writeString(char* data, size_t size, size_t n, void* user) {
    static_cast<std::string*>(user)->append(data, size * n);
    return size * n;
}

struct ProgressBox {
    const Progress* progress;
    long long offset;
    long long expected;
};

int onAbort(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto* abort = static_cast<const std::function<bool()>*>(user);
    return (abort && *abort && (*abort)()) ? 1 : 0;
}

// Only the web: a configured endpoint like file:///... must not read files.
void webOnly(CURL* c, bool httpsOnly) {
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, httpsOnly ? "https" : "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, httpsOnly ? "https" : "http,https");
}

int onProgress(void* user, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* box = static_cast<ProgressBox*>(user);
    if (!box->progress || !*box->progress) return 0;
    const long long t = box->expected > 0 ? box->expected : (total > 0 ? box->offset + total : 0);
    return (*box->progress)(box->offset + now, t) ? 0 : 1;
}

}  // namespace

std::string sha256File(const std::string& path, std::string* error) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { if (error) *error = "cannot read " + path; return {}; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    std::vector<unsigned char> buf(1 << 20);
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) EVP_DigestUpdate(ctx, buf.data(), n);
    std::fclose(f);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, md, &len);
    EVP_MD_CTX_free(ctx);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len; ++i) { out += hex[md[i] >> 4]; out += hex[md[i] & 15]; }
    return out;
}

bool download(const std::string& url, const std::string& dest, long long size,
              const std::string& sha256, const Progress& progress, std::string* error) {
    globalInit();
    const std::string part = dest + ".part";
    long long have = fileSize(part);
    if (have < 0) have = 0;
    if (size > 0 && have > size) { std::remove(part.c_str()); have = 0; }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (size > 0 && have == size) break;   // only the check is left
        FILE* f = std::fopen(part.c_str(), have > 0 ? "ab" : "wb");
        if (!f) { *error = "cannot write " + part; return false; }
        CURL* c = curl_easy_init();
        ProgressBox box{&progress, have, size};
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeFile);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &box);
        curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
        // Give up on a stalled transfer (under 1 KB/s for a minute), not a slow one.
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "basecamp-voice/0.2");
        if (have > 0) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(have));
        useSystemCertificates(c);
        webOnly(c, true);
        const CURLcode rc = curl_easy_perform(c);
        long httpCode = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_easy_cleanup(c);
        std::fclose(f);
        if (rc == CURLE_ABORTED_BY_CALLBACK) { *error = "stopped"; return false; }
        if (rc == CURLE_RANGE_ERROR || (have > 0 && httpCode == 200)) {
            // The server ignored the range: start over.
            std::remove(part.c_str());
            have = 0;
            continue;
        }
        if (rc != CURLE_OK) {
            *error = std::string(errbuf[0] ? errbuf : curl_easy_strerror(rc));
            return false;
        }
        break;
    }

    const long long got = fileSize(part);
    if (size > 0 && got != size) {
        *error = "downloaded " + std::to_string(got) + " bytes, expected " + std::to_string(size);
        if (got > size) std::remove(part.c_str());
        return false;
    }
    if (!sha256.empty()) {
        std::string err;
        const std::string sum = sha256File(part, &err);
        if (sum != sha256) {
            std::remove(part.c_str());
            *error = "checksum mismatch (got " + sum.substr(0, 12) + "..., expected " + sha256.substr(0, 12) + "...)";
            return false;
        }
    }
    if (std::rename(part.c_str(), dest.c_str()) != 0) { *error = "cannot move " + part + " into place"; return false; }
    return true;
}

bool request(const std::string& method, const std::string& url, const std::string& body,
             const std::vector<std::string>& headers, int timeoutSec,
             long* status, std::string* response, std::string* error,
             const std::function<bool()>& abort) {
    globalInit();
    CURL* c = curl_easy_init();
    struct curl_slist* hs = nullptr;
    for (const auto& h : headers) hs = curl_slist_append(hs, h.c_str());
    char errbuf[CURL_ERROR_SIZE] = {0};
    response->clear();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    if (!body.empty() || method == "POST") {
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeString);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, response);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeoutSec));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    if (abort) {
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onAbort);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &abort);
    }
    useSystemCertificates(c);
    webOnly(c, false);
    const CURLcode rc = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, status);
    curl_slist_free_all(hs);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) {
        *error = std::string(errbuf[0] ? errbuf : curl_easy_strerror(rc));
        return false;
    }
    return true;
}

}  // namespace net
