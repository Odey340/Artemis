#pragma once
// Kalshi request signing.
//
//   message   = <timestamp_ms><HTTP_METHOD><path without query string>
//   signature = base64( RSA-PSS-SHA256(message) )   (salt length = digest length)
//               or base64( Ed25519(message) ) for Ed25519 keys
//   headers   = KALSHI-ACCESS-KEY / KALSHI-ACCESS-SIGNATURE / KALSHI-ACCESS-TIMESTAMP
//
// The path includes the "/trade-api/v2" prefix.

#include <memory>
#include <string>
#include <vector>

namespace artemis {

class KalshiSigner {
public:
    // Loads a PEM private key (PKCS#1 or PKCS#8, RSA or Ed25519).
    static std::shared_ptr<KalshiSigner> fromPemFile(const std::string& keyId,
                                                     const std::string& path);
    static std::shared_ptr<KalshiSigner> fromPemString(const std::string& keyId,
                                                       const std::string& pem);
    ~KalshiSigner();

    // Raw signature over `message`, base64 encoded.
    std::string signBase64(const std::string& message) const;

    // Authentication headers for a request. `pathWithQuery` may contain a
    // query string; it is stripped before signing.
    std::vector<std::string> headers(const std::string& method, const std::string& pathWithQuery,
                                     long long timestampMs) const;

    const std::string& keyId() const { return keyId_; }
    bool isEd25519() const { return ed25519_; }

private:
    KalshiSigner() = default;
    std::string keyId_;
    void* pkey_ = nullptr;  // EVP_PKEY*
    bool ed25519_ = false;
};

std::string base64Encode(const unsigned char* data, std::size_t len);
std::vector<unsigned char> base64Decode(const std::string& b64);

}  // namespace artemis
