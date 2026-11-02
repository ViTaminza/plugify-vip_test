// vip_test — where the config lives and how it is read.
//
// The port keeps the original file: the production Metamod install had
// addons/configs/vip/vip_test.ini, which the Plugify configs tree maps to
// <GetConfigsDir()>/vip/vip_test.ini. A standard PLUGIN_STANDARD layout
// (<GetConfigsDir()>/<plugin name>/vip_test.ini) is accepted as a fallback, so a freshly packaged
// plugin works without moving the production file.
//
// The original loaded the file in Load() and refused to start without it. The port fails soft
// (PITFALLS.md A1): a missing or malformed pair logs one error line and the plugin keeps the
// production values (group VIPTEST, time 1800, timeout 172800), exactly like the pre-rewrite port.

#pragma once

#include "common.hpp"

#include <string>
#include <string_view>

namespace viptest {

// The original's szGroup / iTime / iTimeout.
struct VipTestConfig {
	std::string group = kDefaultGroup;
	int time = kDefaultTime;
	int timeout = kDefaultTimeout;
};

// <Plugify configs dir>/<relative>; "" when the host does not provide the configs dir (tests).
std::string ConfigPath(std::string_view relative);

// Reads vip/vip_test.ini, then vip_test/vip_test.ini. Missing files, malformed files or missing
// keys keep the production default of each field; one error line is logged only when neither
// file could be read.
VipTestConfig LoadVipTestConfig();

} // namespace viptest
