#include "grant.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

namespace viptest {

bool ParseTimeStamp(std::string_view text, long long& value) {
	if (text.empty()) {
		return false;
	}
	// strtoll needs a NUL-terminated buffer; a cookie is a short numeric string (std::stoi of the
	// original took a std::string too).
	const std::string buffer(text);
	char* end = nullptr;
	errno = 0;
	const long long parsed = std::strtoll(buffer.c_str(), &end, 10);
	if (end == buffer.c_str() || errno == ERANGE) {
		return false;
	}
	value = parsed;
	return true;
}

Decision Decide(bool isVip, std::string_view cookie, int timeout, std::time_t now) {
	Decision decision;
	if (isVip) {
		decision.outcome = Outcome::AlreadyVip;
		return decision;
	}
	if (cookie.empty()) {
		// !strlen(value): an absent cookie always grants, even with timeout 0 (the original's
		// first condition short-circuits before the timeout check).
		decision.outcome = Outcome::Give;
		return decision;
	}
	long long stored = 0;
	const bool parsed = ParseTimeStamp(cookie, stored);
	decision.invalidCookie = !parsed;
	if (parsed) {
		decision.stored = stored;
	}
	// std::stoi(value) < std::time(0); a value std::stoi would have thrown on counts as expired.
	const bool expired = !parsed || stored < static_cast<long long>(now);
	if (expired && timeout != 0) {
		decision.outcome = Outcome::Give;
		return decision;
	}
	// The else branch of the original: "ReceivedDisabled" when timeout is 0, "AlreadyReceived"
	// otherwise, both with the stored value formatted by the caller.
	decision.outcome = timeout == 0 ? Outcome::ReceivedDisabled : Outcome::AlreadyReceived;
	return decision;
}

long long NextCookieValue(int time, int timeout, std::time_t now) {
	return static_cast<long long>(now) + static_cast<long long>(time) + static_cast<long long>(timeout);
}

std::optional<std::string> FormatLocalTime(long long timestamp) {
	const std::time_t value = static_cast<std::time_t>(timestamp);
	const std::tm* tm = std::localtime(&value);
	if (tm == nullptr) {
		return std::nullopt;
	}
	char buffer[80] = {0};
	if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm) == 0) {
		return std::nullopt;
	}
	return std::string(buffer);
}

std::string CookieToString(const plg::any& value) {
	if (plg::holds_alternative<plg::string>(value)) {
		const plg::string& text = plg::get<plg::string>(value);
		return std::string(text.data(), text.size());
	}
	if (plg::holds_alternative<bool>(value)) {
		return plg::get<bool>(value) ? "1" : "0";
	}
	if (plg::holds_alternative<int64_t>(value)) {
		return std::to_string(plg::get<int64_t>(value));
	}
	if (plg::holds_alternative<int32_t>(value)) {
		return std::to_string(plg::get<int32_t>(value));
	}
	if (plg::holds_alternative<uint64_t>(value)) {
		return std::to_string(plg::get<uint64_t>(value));
	}
	if (plg::holds_alternative<uint32_t>(value)) {
		return std::to_string(plg::get<uint32_t>(value));
	}
	if (plg::holds_alternative<double>(value)) {
		return std::to_string(plg::get<double>(value));
	}
	if (plg::holds_alternative<float>(value)) {
		return std::to_string(plg::get<float>(value));
	}
	return {};
}

} // namespace viptest
