// sas_token.hpp — Azure Event Hubs / Service Bus SAS token construction.
//
// The whole point of the load test is to measure GATEWAY auth overhead, not
// CLIENT auth overhead, so we compute the token exactly once and then reuse the
// identical header value on every request. That mirrors how a well-written
// producer behaves (cache the token until close to expiry).
#pragma once

#include <cstdint>
#include <string>

namespace ehlt {

// Build a SAS token of the canonical form:
//   SharedAccessSignature sr=<uri>&sig=<sig>&se=<expiry>&skn=<keyName>
// where sig = base64(HMAC-SHA256(key, urlencode(uri) + "\n" + expiry)).
//
// `now_epoch_s` is injected (not read from the clock here) so the caller owns
// time and the function stays pure/testable. `expiry = now + ttl`.
std::string make_sas_token(const std::string& uri,
                           const std::string& key_name,
                           const std::string& key,
                           std::uint64_t now_epoch_s,
                           std::uint64_t ttl_s);

}  // namespace ehlt
