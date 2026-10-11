#include "venues/kalshi/KalshiSigner.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace artemis {

namespace {

std::string opensslError(const char* what) {
    unsigned long e = ERR_get_error();
    char buf[256] = {0};
    if (e) ERR_error_string_n(e, buf, sizeof(buf));
    return std::string(what) + (e ? std::string(": ") + buf : std::string());
}

}  // namespace

std::string base64Encode(const unsigned char* data, std::size_t len) {
    std::string out(4 * ((len + 2) / 3), '\0');
    const int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), data,
                                  static_cast<int>(len));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::vector<unsigned char> base64Decode(const std::string& b64) {
    std::vector<unsigned char> out(3 * b64.size() / 4 + 3);
    const int n = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(b64.data()),
                                  static_cast<int>(b64.size()));
    if (n < 0) throw std::runtime_error("invalid base64");
    std::size_t len = static_cast<std::size_t>(n);
    // EVP_DecodeBlock does not account for '=' padding
    if (!b64.empty() && b64.back() == '=') --len;
    if (b64.size() > 1 && b64[b64.size() - 2] == '=') --len;
    out.resize(len);
    return out;
}

std::shared_ptr<KalshiSigner> KalshiSigner::fromPemString(const std::string& keyId,
                                                          const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) throw std::runtime_error("BIO_new_mem_buf failed");
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) throw std::runtime_error(opensslError("could not parse Kalshi private key"));

    const int type = EVP_PKEY_base_id(pkey);
    if (type != EVP_PKEY_RSA && type != EVP_PKEY_ED25519) {
        EVP_PKEY_free(pkey);
        throw std::runtime_error("Kalshi private key must be RSA or Ed25519");
    }
    std::shared_ptr<KalshiSigner> s(new KalshiSigner());
    s->keyId_ = keyId;
    s->pkey_ = pkey;
    s->ed25519_ = (type == EVP_PKEY_ED25519);
    return s;
}

std::shared_ptr<KalshiSigner> KalshiSigner::fromPemFile(const std::string& keyId,
                                                        const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open Kalshi private key file: " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return fromPemString(keyId, ss.str());
}

KalshiSigner::~KalshiSigner() {
    if (pkey_) EVP_PKEY_free(static_cast<EVP_PKEY*>(pkey_));
}

std::string KalshiSigner::signBase64(const std::string& message) const {
    EVP_PKEY* pkey = static_cast<EVP_PKEY*>(pkey_);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    EVP_PKEY_CTX* pctx = nullptr;
    const EVP_MD* md = ed25519_ ? nullptr : EVP_sha256();
    if (EVP_DigestSignInit(ctx, &pctx, md, nullptr, pkey) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error(opensslError("EVP_DigestSignInit"));
    }
    if (!ed25519_) {
        if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
            EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST) != 1 ||
            EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256()) != 1) {
            EVP_MD_CTX_free(ctx);
            throw std::runtime_error(opensslError("configuring RSA-PSS"));
        }
    }
    size_t sigLen = 0;
    const auto* msg = reinterpret_cast<const unsigned char*>(message.data());
    if (EVP_DigestSign(ctx, nullptr, &sigLen, msg, message.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error(opensslError("EVP_DigestSign (size)"));
    }
    std::vector<unsigned char> sig(sigLen);
    if (EVP_DigestSign(ctx, sig.data(), &sigLen, msg, message.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error(opensslError("EVP_DigestSign"));
    }
    EVP_MD_CTX_free(ctx);
    return base64Encode(sig.data(), sigLen);
}

std::vector<std::string> KalshiSigner::headers(const std::string& method,
                                               const std::string& pathWithQuery,
                                               long long timestampMs) const {
    const std::string path = pathWithQuery.substr(0, pathWithQuery.find('?'));
    const std::string ts = std::to_string(timestampMs);
    const std::string sig = signBase64(ts + method + path);
    return {"KALSHI-ACCESS-KEY: " + keyId_, "KALSHI-ACCESS-SIGNATURE: " + sig,
            "KALSHI-ACCESS-TIMESTAMP: " + ts};
}

}  // namespace artemis
