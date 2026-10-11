#pragma once
// Minimal libcurl wrapper. One instance per thread (curl easy handles are not
// thread-safe); connection re-use keeps TLS handshakes off the hot path.

#include <memory>
#include <string>
#include <vector>

namespace artemis {

struct HttpResponse {
    long status = 0;          // 0 => transport error (see `error`)
    std::string body;
    std::string error;
    double elapsedMs = 0.0;
    double retryAfterSec = 0.0;  // parsed Retry-After header, if any

    bool ok() const { return status >= 200 && status < 300; }
    bool transportError() const { return status == 0; }
    bool retryable() const { return status == 0 || status == 429 || status >= 500; }
};

class HttpClient {
public:
    explicit HttpClient(int timeoutMs = 5000, std::string userAgent = "artemis-arb/2.0");
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse request(const std::string& method, const std::string& url,
                         const std::vector<std::string>& headers = {},
                         const std::string& body = {});

    HttpResponse get(const std::string& url, const std::vector<std::string>& headers = {}) {
        return request("GET", url, headers);
    }

    // URL-encode a query component.
    static std::string escape(const std::string& s);

    // Call once from main() before any thread starts.
    static void globalInit();
    static void globalCleanup();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace artemis
