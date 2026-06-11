#include "sas_token.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ehlt {

namespace {

// RFC 3986 percent-encoding. Azure expects the resource URI (and the signature)
// to be form-url-encoded; we encode everything that is not an unreserved char.
std::string url_encode(const std::string& in) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char ch : in) {
        const bool unreserved =
            (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

std::string base64(const unsigned char* data, std::size_t len) {
    // EVP_EncodeBlock writes ceil(len/3)*4 bytes plus a NUL terminator.
    std::string out;
    out.resize(4 * ((len + 2) / 3));
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data,
                            static_cast<int>(len));
    out.resize(static_cast<std::size_t>(n));
    return out;
}

std::string hmac_sha256(const std::string& key, const std::string& msg) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
    unsigned int mac_len = 0;
    const unsigned char* res = HMAC(
        EVP_sha256(),
        key.data(), static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
        mac.data(), &mac_len);
    if (res == nullptr) {
        throw std::runtime_error("HMAC-SHA256 failed");
    }
    return base64(mac.data(), mac_len);
}

}  // namespace

std::string make_sas_token(const std::string& uri,
                           const std::string& key_name,
                           const std::string& key,
                           std::uint64_t now_epoch_s,
                           std::uint64_t ttl_s) {
    const std::uint64_t expiry = now_epoch_s + ttl_s;
    const std::string encoded_uri = url_encode(uri);

    // string-to-sign = url_encode(uri) + "\n" + expiry
    const std::string to_sign = encoded_uri + "\n" + std::to_string(expiry);
    const std::string sig = url_encode(hmac_sha256(key, to_sign));

    return "SharedAccessSignature sr=" + encoded_uri +
           "&sig=" + sig +
           "&se=" + std::to_string(expiry) +
           "&skn=" + url_encode(key_name);
}

}  // namespace ehlt
