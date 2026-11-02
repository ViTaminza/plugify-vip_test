#include "config.hpp"

#include "common.hpp"
#include "kv1.hpp"
#include "log.hpp"
#include "text.hpp"

#include <plg/plugin.hpp>

#include <optional>
#include <string>

namespace viptest {
namespace {

// The three keys of the original's KeyValues block:
//   "group" (GetString), "time" (GetInt), "timeout" (GetInt).
VipTestConfig ReadConfig(const KvNode& root) {
	VipTestConfig config;
	config.group = std::string(root.GetString("group", kDefaultGroup));
	config.time = root.GetInt("time", kDefaultTime);
	config.timeout = root.GetInt("timeout", kDefaultTimeout);
	return config;
}

} // namespace

std::string ConfigPath(std::string_view relative) {
	if (!plg::GetConfigsDir) {
		return {};
	}
	std::string path(plg::GetConfigsDir());
	if (path.empty()) {
		return {};
	}
	if (path.back() != '/') {
		path.push_back('/');
	}
	path.append(relative);
	return path;
}

VipTestConfig LoadVipTestConfig() {
	const std::string primary = ConfigPath(kConfigFile);
	if (primary.empty()) {
		LogError(Format("[Error] [%s] The Plugify configs directory is unavailable; using group %s, time %i, "
						"timeout %i",
						kLogTag, kDefaultGroup, kDefaultTime, kDefaultTimeout));
		return {};
	}
	if (const std::optional<KvNode> file = LoadKv1File(primary)) {
		return ReadConfig(*file);
	}
	const std::string fallback = ConfigPath(kConfigFileFallback);
	if (!fallback.empty()) {
		if (const std::optional<KvNode> file = LoadKv1File(fallback)) {
			return ReadConfig(*file);
		}
	}
	LogError(Format("[Error] [%s] Failed to load %s (using group %s, time %i, timeout %i)", kLogTag,
					primary.c_str(), kDefaultGroup, kDefaultTime, kDefaultTimeout));
	return {};
}

} // namespace viptest
