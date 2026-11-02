// vip_test — the one log sink of the plugin.
//
// pisexcompat's ErrorLog prefixed every line with "[Error] "; the callers keep that prefix in the
// text itself (so the log line is byte-identical to the pre-rewrite plugin), and this wrapper only
// guards against a host that runs with logging disabled (plg::Log is then null).

#pragma once

#include <string_view>

namespace viptest {

void LogError(std::string_view message);

} // namespace viptest
