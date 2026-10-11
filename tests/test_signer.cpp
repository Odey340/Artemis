#include <gtest/gtest.h>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <memory>
#include <string>

#include "venues/kalshi/KalshiSigner.hpp"

using namespace artemis;

namespace {

struct Key {
    std::string privPem;
    std::string pubPem;
};

Key generate(int type) {
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(type, nullptr);
    EVP_PKEY_keygen_init(ctx);
    if (type == EVP_PKEY_RSA) EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);
    EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);

    Key k;
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(b, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    char* data = nullptr;
    long len = BIO_get_mem_data(b, &data);
    k.privPem.assign(data, static_cast<size_t>(len));
    BIO_free(b);
    b = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(b, pkey);
    len = BIO_get_mem_data(b, &data);
    k.pubPem.assign(data, static_cast<size_t>(len));
    BIO_free(b);
    EVP_PKEY_free(pkey);
    return k;
}

bool verify(const std::string& pubPem, const std::string& msg, const std::string& sigB64, bool rsa) {
    BIO* b = BIO_new_mem_buf(pubPem.data(), static_cast<int>(pubPem.size()));
    EVP_PKEY* pub = PEM_read_bio_PUBKEY(b, nullptr, nullptr, nullptr);
    BIO_free(b);
    const auto sig = base64Decode(sigB64);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_PKEY_CTX* pctx = nullptr;
    EVP_DigestVerifyInit(ctx, &pctx, rsa ? EVP_sha256() : nullptr, nullptr, pub);
    if (rsa) {
        EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING);
        EVP_PKEY_CTX_set_rsa_pss_saltlen(pctx, RSA_PSS_SALTLEN_DIGEST);
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, EVP_sha256());
    }
    const int ok = EVP_DigestVerify(ctx, sig.data(), sig.size(),
                                    reinterpret_cast<const unsigned char*>(msg.data()), msg.size());
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pub);
    return ok == 1;
}

std::string header(const std::vector<std::string>& h, const std::string& name) {
    for (const auto& x : h)
        if (x.rfind(name + ": ", 0) == 0) return x.substr(name.size() + 2);
    return {};
}

}  // namespace

TEST(KalshiSigner, RsaPssSignatureVerifies) {
    const Key k = generate(EVP_PKEY_RSA);
    auto s = KalshiSigner::fromPemString("key-123", k.privPem);
    EXPECT_FALSE(s->isEd25519());
    const auto h = s->headers("GET", "/trade-api/v2/portfolio/balance?x=1", 1700000000123LL);
    EXPECT_EQ(header(h, "KALSHI-ACCESS-KEY"), "key-123");
    EXPECT_EQ(header(h, "KALSHI-ACCESS-TIMESTAMP"), "1700000000123");
    // query string must NOT be part of the signed message
    EXPECT_TRUE(verify(k.pubPem, "1700000000123GET/trade-api/v2/portfolio/balance",
                       header(h, "KALSHI-ACCESS-SIGNATURE"), true));
    EXPECT_FALSE(verify(k.pubPem, "1700000000123GET/trade-api/v2/portfolio/balance?x=1",
                        header(h, "KALSHI-ACCESS-SIGNATURE"), true));
    // PSS is randomised: two signatures differ but both verify
    EXPECT_NE(s->signBase64("m"), s->signBase64("m"));
}

TEST(KalshiSigner, Ed25519SignatureVerifies) {
    const Key k = generate(EVP_PKEY_ED25519);
    auto s = KalshiSigner::fromPemString("ed", k.privPem);
    EXPECT_TRUE(s->isEd25519());
    const auto sig = s->signBase64("1POST/trade-api/v2/portfolio/events/orders");
    EXPECT_TRUE(verify(k.pubPem, "1POST/trade-api/v2/portfolio/events/orders", sig, false));
}

TEST(KalshiSigner, RejectsBadKeys) {
    EXPECT_THROW(KalshiSigner::fromPemString("k", "not a key"), std::runtime_error);
    EXPECT_THROW(KalshiSigner::fromPemFile("k", "/nonexistent/key.pem"), std::runtime_error);
}

TEST(Base64, RoundTrip) {
    for (std::string s : {"", "a", "ab", "abc", "abcd", "hello world!"}) {
        const std::string enc =
            base64Encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
        const auto dec = base64Decode(enc);
        EXPECT_EQ(std::string(dec.begin(), dec.end()), s);
    }
    EXPECT_EQ(base64Encode(reinterpret_cast<const unsigned char*>("ab"), 2), "YWI=");
}
