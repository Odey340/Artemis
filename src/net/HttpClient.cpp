#include "net/HttpClient.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace artemis {

namespace {

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t headerCb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* resp = static_cast<HttpResponse*>(userdata);
    const size_t n = size * nitems;
    static const char kKey[] = "retry-after:";
    if (n > sizeof(kKey) - 1) {
        bool match = true;
        for (size_t i = 0; i < sizeof(kKey) - 1; ++i) {
            char c = buffer[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != kKey[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            std::string v(buffer + sizeof(kKey) - 1, n - (sizeof(kKey) - 1));
            resp->retryAfterSec = std::atof(v.c_str());
        }
    }
    return n;
}

}  // namespace

struct HttpClient::Impl {
    CURL* curl = nullptr;
    int timeoutMs = 5000;
    std::string userAgent;
};

void HttpClient::globalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void HttpClient::globalCleanup() { curl_global_cleanup(); }

HttpClient::HttpClient(int timeoutMs, std::string userAgent) : impl_(std::make_unique<Impl>()) {
    impl_->curl = curl_easy_init();
    if (!impl_->curl) throw std::runtime_error("curl_easy_init failed");
    impl_->timeoutMs = timeoutMs;
    impl_->userAgent = std::move(userAgent);
}

HttpClient::~HttpClient() {
    if (impl_ && impl_->curl) curl_easy_cleanup(impl_->curl);
}

std::string HttpClient::escape(const std::string& s) {
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

HttpResponse HttpClient::request(const std::string& method, const std::string& url,
                                 const std::vector<std::string>& headers,
                                 const std::string& body) {
    HttpResponse resp;
    CURL* c = impl_->curl;
    curl_easy_reset(c);

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_USERAGENT, impl_->userAgent.c_str());
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(impl_->timeoutMs));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(impl_->timeoutMs));
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");  // gzip/deflate/br if available
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &resp);

    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());
    hdrs = curl_slist_append(hdrs, "Accept: application/json");
    if (!body.empty()) hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);

    if (method == "GET") {
        curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    } else if (method == "POST") {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    } else {
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(c);
    resp.elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    if (rc != CURLE_OK) {
        resp.status = 0;
        resp.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp.status);
    }
    curl_slist_free_all(hdrs);
    return resp;
}

}  // namespace artemis
