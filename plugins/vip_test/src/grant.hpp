// vip_test — the decision of the original's VIP_Test() handler.
//
// The original (ref/src/cs2-vip-modules/VIP_Test/source/vip_test.cpp) is three lines of business
// logic wrapped in client and cookie calls; this module keeps exactly those three lines pure and
// testable, while plugin.cpp does the s2sdk/vip/permissions wiring:
//
//   if (!VIP_IsClientVIP(slot)) {
//       const char* value = VIP_GetClientCookie(slot, "vip_test");
//       if (!strlen(value) || ((std::stoi(value) < std::time(0)) && iTimeout != 0)) {
//           VIP_SetClientCookie(slot, "vip_test", std::to_string(std::time(0) + iTime + iTimeout));
//           VIP_GiveClientVIP(slot, iTime, szGroup);
//       } else {
//           // "AlreadyReceived" / "ReceivedDisabled" with the stored expiry as a date
//       }
//   } else {
//       // "AlreadyVIP"
//   }
//
// Port notes:
//   * std::stoi of the original threw on a malformed cookie; the port parses non-throwing and
//     reports the value as invalid, so a corrupted cookie cannot take the server down from a
//     command handler (the caller logs it and treats the value as expired, PITFALLS A1);
//   * std::localtime of the original could return NULL for a hand-edited cookie; the caller then
//     prints the raw number instead of passing NULL to std::strftime.

#pragma once

#include <plg/any.hpp>

#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace viptest {

// What the original's handler did for one call.
enum class Outcome {
	Give,             // write the cookie and call VIP_GiveClientVIP
	AlreadyReceived,  // the "AlreadyReceived" phrase with the stored expiry
	ReceivedDisabled, // the "ReceivedDisabled" phrase with the stored expiry (timeout == 0)
	AlreadyVip,       // the "AlreadyVIP" phrase
};

struct Decision {
	Outcome outcome = Outcome::Give;
	// The value the report phrases print: the stored cookie read as a number, 0 when it was not
	// one (the original's std::stoi would have thrown there; the port prints the epoch).
	long long stored = 0;
	// The cookie was non-empty but did not start with a number. The caller logs it once.
	bool invalidCookie = false;
};

// std::stoi of the original without the throw: leading whitespace is skipped and a trailing tail
// ("1800 // x", "12x") is ignored, exactly like std::stoi/strtoll; false when there is no number.
bool ParseTimeStamp(std::string_view text, long long& value);

// The handler's decision: an empty cookie grants; a stored value older than `now` grants while
// timeout != 0; every other case reports. `now` is std::time(0) (the original called it twice;
// the caller of this helper takes one timestamp for both the decision and the new cookie).
Decision Decide(bool isVip, std::string_view cookie, int timeout, std::time_t now);

// The value the grant stores: std::to_string(std::time(0) + iTime + iTimeout).
long long NextCookieValue(int time, int timeout, std::time_t now);

// The stored expiry as "%Y-%m-%d %H:%M:%S" local time; nullopt when std::localtime cannot
// represent it, so the caller prints the raw number instead.
std::optional<std::string> FormatLocalTime(long long timestamp);

// The core's own conversion of a stored cookie value to the text form the handler parses.
std::string CookieToString(const plg::any& value);

} // namespace viptest
