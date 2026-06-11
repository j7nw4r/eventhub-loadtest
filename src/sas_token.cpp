#include "sas_token.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace ehlt {

namespace {

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif

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

// Base64 via crypt32. CryptBinaryToStringA wants the buffer size (including the
// NUL) on the sizing call and reports the written length (excluding the NUL) on
// the real call, so we resize twice.
std::string base64(const unsigned char* data, std::size_t len) {
    DWORD sz = 0;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(len),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &sz)) {
        throw std::runtime_error("CryptBinaryToStringA (sizing) failed");
    }
    std::string out(sz, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(len),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              out.data(), &sz)) {
        throw std::runtime_error("CryptBinaryToStringA failed");
    }
    out.resize(sz);  // sz is now the length without the NUL terminator
    return out;
}

// HMAC-SHA256 via CNG (bcrypt). The key is used verbatim: Azure Service Bus /
// Event Hubs sign with the SharedAccessKey string itself as the HMAC secret,
// NOT a base64-decoded form of it, so the output matches the canonical token.
std::string hmac_sha256(const std::string& key, const std::string& msg) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<unsigned char> obj, mac;
    DWORD cbObj = 0, cbHash = 0, cbData = 0;
    NTSTATUS st = 0;

    auto fail = [&](const char* what) -> std::string {
        if (hHash) BCryptDestroyHash(hHash);
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
        throw std::runtime_error(what);
    };

    st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                     BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!NT_SUCCESS(st)) return fail("BCryptOpenAlgorithmProvider failed");

    st = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                           reinterpret_cast<PUCHAR>(&cbObj), sizeof(cbObj),
                           &cbData, 0);
    if (!NT_SUCCESS(st)) return fail("BCryptGetProperty(OBJECT_LENGTH) failed");
    obj.resize(cbObj);

    st = BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH,
                           reinterpret_cast<PUCHAR>(&cbHash), sizeof(cbHash),
                           &cbData, 0);
    if (!NT_SUCCESS(st)) return fail("BCryptGetProperty(HASH_LENGTH) failed");
    mac.resize(cbHash);

    st = BCryptCreateHash(
        hAlg, &hHash, obj.data(), cbObj,
        reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
        static_cast<ULONG>(key.size()), 0);
    if (!NT_SUCCESS(st)) return fail("BCryptCreateHash failed");

    st = BCryptHashData(
        hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(msg.data())),
        static_cast<ULONG>(msg.size()), 0);
    if (!NT_SUCCESS(st)) return fail("BCryptHashData failed");

    st = BCryptFinishHash(hHash, mac.data(), cbHash, 0);
    if (!NT_SUCCESS(st)) return fail("BCryptFinishHash failed");

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return base64(mac.data(), mac.size());
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
