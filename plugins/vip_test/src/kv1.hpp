// vip_test — a read-only Valve KeyValues (KV1) reader for the plugin's config.
//
// The production config is the original Metamod file, unchanged:
//
//   "VIP"
//   {
//       "group"     "VIPTEST"
//       "time"      "1800"
//       "timeout"   "172800"
//   }
//
// The grammar is the one pisexcompat's KeyValues accepted (the same code s2guard shipped, now in
// the plugin itself, PLUGIN_STANDARD section 7):
//   * quoted ("key" "value") and unquoted tokens, nested "key" { ... } blocks, a key may carry a
//     value and a block,
//   * // and /* */ comments, escapes \" \\ \n \r \t inside quotes,
//   * #base / #include / #define lines are skipped (no inclusion),
//   * an unterminated string, a missing '}' or a stray top-level '}' makes the file invalid
//     (nullopt; the caller keeps its default instead of a half-read tree).
// Like Valve's loader, a file that is a single "Root" { ... } block is returned as that block, so
// `ParseKv1(text)->GetString("group")` reads the key inside it. Key lookup is case-insensitive.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viptest {

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or has no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int, with Valve's atoi fallback ("5 // x" -> 5, "abc" -> 0); `def`
	// when the child is missing or its value is empty.
	int GetInt(std::string_view key, int def = 0) const;
};

std::optional<KvNode> ParseKv1(std::string_view text);
std::optional<KvNode> LoadKv1File(const std::string& path);

} // namespace viptest
