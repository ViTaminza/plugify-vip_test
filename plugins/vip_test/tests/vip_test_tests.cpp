// vip_test_tests — the engine-free test binary of the pure-s2sdk vip_test plugin.
//
// The plugin TU itself is compiled into the test (like vip_speed's test), so the whole lifecycle —
// OnPluginStart / OnPluginUpdate / OnPluginEnd and the handler — runs exactly as on a server, with
// every boundary a recording fake: the __s2sdk_* client/map/command slots, the __vip_* slots of the
// vip core and the __permissions_* cookie slots. There is no pisexcompat anywhere in the build of
// this test.
//
// Covered:
//   1. The pure helpers (no binding touched): the KV1 reader on the production vip_test.ini, the
//      cs2-menus colour table, the printf-format phrase semantics (FormatPhrase), the bare chat
//      words and the cooldown decision of the original handler (moved out of the command handler
//      into grant.cpp so it is testable without an engine).
//   2. The config reader: vip/vip_test.ini first, vip_test/vip_test.ini as the PLUGIN_STANDARD
//      fallback, one error line and the production values when neither can be read.
//   3. The plugin's own guards: no client query before the first map or after OnMapEnd (PITFALLS
//      B1/B2), HasClient() layers slot/connected/in-game/SourceTV/bot (C2, B5), ClientSteamID64()
//      only for an accepted client, the guarded and colourised PrintToChat (C2), and a clean
//      ShutdownMapGate().
//   4. The command dispatch (C11): console-only mm_/sm_ names, chat-only "!viptest"/"!testvip",
//      the bare words on say/say_team, the source filter, and a clean RemoveCommands() (A7).
//   5. The plugin flow with fake vip/permissions bindings: the commands appear only after
//      VIP_OnVIPLoaded, the first call grants (cookie now + time + timeout, GiveClientVIP), the
//      second reports the stored expiry, timeout 0 answers ReceivedDisabled while an empty cookie
//      still grants, a VIP gets AlreadyVIP, a bot or the server console never reaches s2sdk, and a
//      start without the vip bindings stays harmless (PITFALLS A1). A clean unload is checked too.
//
// Run by ../../test.sh (build/tests/vip_test_tests), or directly.

#include "../src/plugin.cpp"

#include <plg/plugin.hpp>
#include <plg/vector.hpp>
#include <s2sdk/s2sdk.hpp>

#include "chat_words.hpp"
#include "commands.hpp"
#include "common.hpp"
#include "config.hpp"
#include "grant.hpp"
#include "guards.hpp"
#include "kv1.hpp"
#include "text.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

// Non-printable bytes are shown as \xNN: the colour output is mostly control bytes.
std::string Printable(std::string_view text) {
	std::string out;
	for (const unsigned char c : text) {
		if (c >= 0x20 && c < 0x7f) {
			out.push_back(static_cast<char>(c));
		} else {
			char buffer[8];
			std::snprintf(buffer, sizeof(buffer), "\\x%02X", c);
			out.append(buffer);
		}
	}
	return out;
}

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void CheckEq(std::string_view actual, std::string_view expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected \"%s\", got \"%s\")\n", what.c_str(), Printable(expected).c_str(),
					Printable(actual).c_str());
	}
}

void CheckEqInt(long long actual, long long expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %lld, got %lld)\n", what.c_str(), expected, actual);
	}
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

plg::string ToPlg(const std::string& text) {
	return plg::string(text.data(), text.size());
}

// --- the plugify host fakes -------------------------------------------------------------------

std::vector<std::string> g_logLines;
plg::Severity g_lastSeverity = plg::Severity::Unknown;
std::string g_configsDir;

void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	g_logLines.emplace_back(message);
	g_lastSeverity = severity;
}

plg::string FakeGetConfigsDir() {
	return ToPlg(g_configsDir);
}

// The id of this plugin for permissions::SetCookie; any non-zero value works.
constexpr int64_t kPluginId = 7;

plg::uuid FakeGetId(plg::hndl /*plugin*/) {
	return static_cast<plg::uuid>(kPluginId);
}

// ==============================================================================================
// Part 1: the pure helpers
// ==============================================================================================

// The production vip_test.ini, byte for byte.
constexpr std::string_view kIniProduction = R"ini("VIP"
{
    "group"     "VIPTEST"
    "time"      "1800"
    "timeout"   "172800"
}
)ini";

void TestKv1() {
	// The shipped file shape: a single "VIP" block, case-insensitive key lookup.
	const std::optional<viptest::KvNode> ini = viptest::ParseKv1(kIniProduction);
	Check(ini.has_value(), "vip_test.ini parses");
	if (ini) {
		CheckEq(ini->GetString("group"), "VIPTEST", "vip_test.ini group");
		CheckEqInt(ini->GetInt("time", -1), 1800, "vip_test.ini time = 1800");
		CheckEqInt(ini->GetInt("timeout", -1), 172800, "vip_test.ini timeout = 172800");
		CheckEq(ini->GetString("GROUP"), "VIPTEST", "vip_test.ini key lookup is case-insensitive");
		CheckEq(ini->GetString("missing", "fallback"), "fallback", "GetString default");
		CheckEqInt(ini->GetInt("missing", 7), 7, "GetInt default");
	}

	// Comments, #base lines, unquoted tokens and a nested block.
	const std::optional<viptest::KvNode> grammar = viptest::ParseKv1(R"(
#base "something.txt" // skipped, never included
VIP
{
    group VIPTEST
    time 60 // the free period
    outer /* block comment */ { inner "x" }
    "tail" "y" // trailing comment
}
)");
	Check(grammar.has_value(), "unquoted tokens / comments / nested blocks parse");
	if (grammar) {
		CheckEq(grammar->GetString("group"), "VIPTEST", "unquoted group value");
		CheckEqInt(grammar->GetInt("time", -1), 60, "unquoted time value");
		const viptest::KvNode* outer = grammar->Find("outer");
		Check(outer != nullptr && outer->Find("inner") != nullptr && outer->GetString("inner") == "x",
			  "nested block");
		CheckEq(grammar->GetString("tail"), "y", "quoted key/value with a trailing comment");
	}

	// Escapes inside quoted tokens.
	const std::optional<viptest::KvNode> escapes =
			viptest::ParseKv1(R"("VIP" { "group" "a\"b\\c\td" "time" "1" })");
	Check(escapes.has_value() && escapes->GetString("group") == std::string("a\"b\\c\td"), "quoted escapes");
	Check(escapes.has_value() && escapes->GetInt("time", -1) == 1, "escaped sibling key");

	// CRLF files (the production config came from a Windows checkout once).
	const std::optional<viptest::KvNode> crlf =
			viptest::ParseKv1("\"VIP\"\r\n{\r\n    \"time\"    \"60\"\r\n}\r\n");
	Check(crlf.has_value() && crlf->GetInt("time", -1) == 60, "CRLF file");

	// Valve's atoi fallback ("abc" -> 0, "5x" -> 5).
	const std::optional<viptest::KvNode> atoi = viptest::ParseKv1("\"VIP\" { \"time\" \"abc\" \"other\" \"5x\" }");
	Check(atoi.has_value() && atoi->GetInt("time", -1) == 0, "non-numeric time falls back to atoi -> 0");
	Check(atoi.has_value() && atoi->GetInt("other", -1) == 5, "partially numeric value -> 5");

	// Malformed input is nullopt, not a half-read tree.
	Check(!viptest::ParseKv1("\"VIP\" { \"time\" \"1\"").has_value(), "missing '}' is invalid");
	Check(!viptest::ParseKv1("\"VIP\" { \"time\" \"1 }").has_value(), "unterminated string is invalid");
	Check(!viptest::ParseKv1("\"VIP\" }").has_value(), "stray top-level '}' is invalid");

	// A file without the single "Root" block wrapper keeps its top level.
	const std::optional<viptest::KvNode> flat = viptest::ParseKv1("\"time\" \"60\"");
	Check(flat.has_value() && flat->GetInt("time", -1) == 60, "flat key/value file");

	// The shipped test data file.
	const std::string path = std::string(VIP_TEST_TEST_DATA_DIR) + "/vip_test.ini";
	const std::optional<viptest::KvNode> file = viptest::LoadKv1File(path);
	Check(file.has_value(), "tests/data/vip_test.ini loads");
	if (file) {
		CheckEq(file->GetString("group"), "VIPTEST", "tests/data group = VIPTEST");
		CheckEqInt(file->GetInt("time", -1), 1800, "tests/data time = 1800");
		CheckEqInt(file->GetInt("timeout", -1), 172800, "tests/data timeout = 172800");
	}
	Check(!viptest::LoadKv1File(std::string(VIP_TEST_TEST_DATA_DIR) + "/missing.ini").has_value(),
		  "a missing file is nullopt");
}

void TestColorize() {
	struct ColorCase {
		const char* tag;
		const char* bytes;
	};
	// cs2-menus utils.h colors_text[]/colors_hex[], the table the Metamod servers sent.
	constexpr ColorCase kCases[] = {
			{"{DEFAULT}", "\x01"},
			{"{WHITE}", "\x01"},
			{"{RED}", "\x02"},
			{"{LIGHTPURPLE}", "\x03"},
			{"{GREEN}", "\x04"},
			{"{LIME}", "\x05"},
			{"{LIGHTGREEN}", "\x06"},
			{"{DARKRED}", "\x07"},
			{"{GRAY}", "\x08"},
			{"{LIGHTOLIVE}", "\x09"},
			{"{OLIVE}", "\x10"},
			{"{LIGHTBLUE}", "\x0B"},
			{"{BLUE}", "\x0C"},
			{"{PURPLE}", "\x0E"},
			{"{LIGHTRED}", "\x0F"},
			{"{GRAYBLUE}", "\x0A"},
			{"\\n", "\xe2\x80\xa9"},
	};
	for (const ColorCase& color : kCases) {
		CheckEq(viptest::Colorize(color.tag), color.bytes, std::string("Colorize ") + color.tag);
	}

	CheckEq(viptest::Colorize("{GREEN}VIP {DEFAULT}already!"), "\x04" "VIP " "\x01" "already!",
			"Colorize a whole VIP phrase");
	CheckEq(viptest::Colorize("plain"), "plain", "Colorize leaves plain text alone");
	CheckEq(viptest::Colorize(""), "", "Colorize of an empty string");
	CheckEq(viptest::Colorize("{MAGENTA}x"), "{MAGENTA}x", "an unknown tag is left alone");
	CheckEq(viptest::Colorize("a\\nb"), "a\xe2\x80\xa9" "b", "the \\n tag is U+2029 in chat");
	CheckEq(viptest::StripColorTags("{GREEN}hi{DEFAULT}"), "hi", "StripColorTags removes tags");
	CheckEq(viptest::StripColorTags("a\\nb"), "a\nb", "StripColorTags turns \\n into a newline");
	CheckEq(viptest::StripColorTags("plain"), "plain", "StripColorTags leaves plain text alone");
}

void TestFormatPhrase() {
	// The "AlreadyReceived" phrase of vip.phrases.txt: the stored expiry is its %s.
	constexpr char kAlreadyReceived[] =
			"Re-acquisition of {GREEN}VIP status {DEFAULT} will be available %s!";
	CheckEq(viptest::FormatPhrase(kAlreadyReceived, "2026-11-01 12:00:00"),
			"Re-acquisition of {GREEN}VIP status {DEFAULT} will be available 2026-11-01 12:00:00!",
			"FormatPhrase substitutes the expiry");
	CheckEq(viptest::FormatPhrase("%s", "Bob"), "Bob", "FormatPhrase %s");
	CheckEq(viptest::FormatPhrase("%d", 42), "42", "FormatPhrase %d");

	// A phrase without arguments is still the printf FORMAT (the original passed it to
	// pisex::PrintToChat, which took it as the format), so "%%" prints "%".
	CheckEq(viptest::FormatPhrase("Re-acquisition is disabled!"), "Re-acquisition is disabled!",
			"FormatPhrase without arguments");
	CheckEq(viptest::FormatPhrase("100%%"), "100%", "FormatPhrase keeps the %% semantics");
	CheckEq(viptest::FormatPhrase(""), "", "FormatPhrase of an empty phrase");
	CheckEq(viptest::FormatPhrase(nullptr), "", "FormatPhrase(nullptr) is empty like the old PrintToChat");

	// Format() is the printf-checked sibling used for the log lines.
	CheckEq(viptest::Format("%s", "Bob"), "Bob", "Format %s");
	CheckEq(viptest::Format("%i", 42), "42", "Format %i");
	CheckEq(viptest::Format("[Error] [%s] %s", "VIP-TEST", "boom"), "[Error] [VIP-TEST] boom",
			"the error-log shape");

	// The GetTranslate() result goes through FormatPhrase, then PrintToChat colorizes it.
	CheckEq(viptest::Colorize(viptest::FormatPhrase("%s", "{RED}Bob{DEFAULT}")), "\x02" "Bob" "\x01",
			"a formatted phrase is colorized afterwards");
}

void TestChatWords() {
	CheckEq(viptest::FirstChatWord("viptest"), "viptest", "FirstChatWord viptest");
	CheckEq(viptest::FirstChatWord("TESTVIP"), "testvip", "FirstChatWord is case-insensitive");
	CheckEq(viptest::FirstChatWord("   viptest"), "viptest", "FirstChatWord skips leading spaces");
	CheckEq(viptest::FirstChatWord("viptest\t\t"), "viptest", "FirstChatWord stops at any whitespace");
	CheckEq(viptest::FirstChatWord("\"viptest\" extra"), "viptest", "quoted first word");
	CheckEq(viptest::FirstChatWord(""), "", "FirstChatWord of an empty line");
	CheckEq(viptest::FirstChatWord("   "), "", "FirstChatWord of whitespace only");

	const std::vector<std::string> words = viptest::SplitChatWords("  one  \"two three\" four ");
	CheckEqInt(static_cast<long long>(words.size()), 3, "SplitChatWords count");
	Check(words.size() == 3 && words[0] == "one" && words[1] == "two three" && words[2] == "four",
		  "SplitChatWords quote grouping");

	Check(viptest::IsBareChatWord("viptest"), "viptest is a bare word");
	Check(!viptest::IsBareChatWord("!viptest"), "!viptest is not a bare word");
	Check(!viptest::IsBareChatWord("/viptest"), "/viptest is not a bare word");
	Check(!viptest::IsBareChatWord(""), "an empty word is not a bare word");

	Check(viptest::MatchesChatWord("viptest", "viptest"), "the bare word matches itself");
	Check(viptest::MatchesChatWord("VIPTEST", "viptest"), "the bare word matches case-insensitively");
	Check(viptest::MatchesChatWord("  testvip  ", "testvip"), "the bare word matches with whitespace");
	Check(viptest::MatchesChatWord("\"testvip\"", "testvip"), "the bare word matches when quoted");
	Check(viptest::MatchesChatWord("viptest top", "viptest"), "only the first word decides");

	Check(!viptest::MatchesChatWord("top viptest", "viptest"), "a later word does not match");
	Check(!viptest::MatchesChatWord("!viptest", "viptest"), "!viptest is the chat command, not the word");
	Check(!viptest::MatchesChatWord("/viptest", "viptest"), "/viptest is the chat command");
	Check(!viptest::MatchesChatWord("sviptest", "viptest"), "a different word does not match");
	Check(!viptest::MatchesChatWord("viptestx", "viptest"), "a longer word does not match");
	Check(!viptest::MatchesChatWord("testvip", "viptest"), "the other spelling does not match");
	Check(!viptest::MatchesChatWord("", "viptest"), "an empty line does not match");
	Check(!viptest::MatchesChatWord("viptest", "!viptest"), "the registered word must itself be bare");

	// The already-tokenised console path (s2sdk split the line itself).
	Check(viptest::MatchesChatWordToken("viptest", "viptest"), "a token matches itself");
	Check(viptest::MatchesChatWordToken("TESTVIP", "testvip"), "a token matches case-insensitively");
	Check(!viptest::MatchesChatWordToken("!viptest", "viptest"), "a trigger token is not a bare word");
	Check(!viptest::MatchesChatWordToken("viptest top", "viptest"), "a token is not split again");
	Check(!viptest::MatchesChatWordToken("", "viptest"), "an empty token does not match");
}

void TestGrantDecision() {
	using viptest::Decide;
	using viptest::Outcome;
	const std::time_t now = 1700000000;

	// A VIP is answered before the cookie is read (the caller skips the read altogether).
	Check(Decide(true, "", 172800, now).outcome == Outcome::AlreadyVip, "a VIP is AlreadyVIP");
	Check(Decide(true, "0", 0, now).outcome == Outcome::AlreadyVip, "a VIP is AlreadyVIP with timeout 0");

	// An empty cookie always grants, even with timeout 0 (the original's !strlen short-circuit).
	Check(Decide(false, "", 172800, now).outcome == Outcome::Give, "an empty cookie grants");
	Check(Decide(false, "", 0, now).outcome == Outcome::Give, "an empty cookie grants with timeout 0");
	Check(!Decide(false, "", 0, now).invalidCookie, "an empty cookie is not an invalid one");

	// A stored value in the past grants while timeout != 0 and reports when timeout == 0.
	Check(Decide(false, "1699999999", 172800, now).outcome == Outcome::Give, "an expired cookie grants");
	Check(Decide(false, "1699999999", 0, now).outcome == Outcome::ReceivedDisabled,
		  "an expired cookie reports ReceivedDisabled with timeout 0");
	CheckEqInt(Decide(false, "1699999999", 0, now).stored, 1699999999,
			   "the report keeps the stored value");

	// The boundary: exactly now is not expired (std::stoi(value) < std::time(0) is strict).
	Check(Decide(false, "1700000000", 172800, now).outcome == Outcome::AlreadyReceived,
		  "a value equal to now is not expired");
	CheckEqInt(Decide(false, "1700000000", 172800, now).stored, 1700000000, "the stored boundary value");
	Check(Decide(false, "1700000001", 172800, now).outcome == Outcome::AlreadyReceived,
		  "a value in the future reports AlreadyReceived");
	Check(Decide(false, "1700000001", 0, now).outcome == Outcome::ReceivedDisabled,
		  "a future value reports ReceivedDisabled with timeout 0");

	// std::stoi accepts a trailing tail ("1700000001x" -> 1700000001); the value decides.
	Check(Decide(false, "1700000001x", 172800, now).outcome == Outcome::AlreadyReceived,
		  "a trailing tail is ignored like std::stoi");
	Check(!Decide(false, "1700000001x", 172800, now).invalidCookie, "a trailing tail is not invalid");

	// A value std::stoi would have thrown on: treated as expired and reported to the caller.
	const viptest::Decision bad = Decide(false, "not-a-number", 172800, now);
	Check(bad.outcome == Outcome::Give, "an unparseable cookie grants (treated as expired)");
	Check(bad.invalidCookie, "the unparseable cookie is reported");
	const viptest::Decision badZero = Decide(false, "not-a-number", 0, now);
	Check(badZero.outcome == Outcome::ReceivedDisabled, "an unparseable cookie reports with timeout 0");
	Check(badZero.invalidCookie, "the unparseable cookie is reported with timeout 0");
	CheckEqInt(badZero.stored, 0, "an unparseable cookie has no stored value");

	// The value a grant stores: std::to_string(std::time(0) + iTime + iTimeout).
	CheckEqInt(viptest::NextCookieValue(1800, 172800, now), 1700000000LL + 1800 + 172800,
			   "NextCookieValue adds time and timeout");
	CheckEqInt(viptest::NextCookieValue(0, 0, now), 1700000000, "NextCookieValue with time 0, timeout 0");

	// ParseTimeStamp mirrors std::stoi: leading whitespace, sign and a trailing tail; no number at
	// all (or an out-of-range one) is false.
	long long value = -1;
	Check(viptest::ParseTimeStamp("1800", value) && value == 1800, "ParseTimeStamp plain");
	Check(viptest::ParseTimeStamp(" 42", value) && value == 42, "ParseTimeStamp leading whitespace");
	Check(viptest::ParseTimeStamp("+7", value) && value == 7, "ParseTimeStamp plus sign");
	Check(viptest::ParseTimeStamp("-5", value) && value == -5, "ParseTimeStamp minus sign");
	Check(viptest::ParseTimeStamp("12x", value) && value == 12, "ParseTimeStamp trailing tail");
	Check(!viptest::ParseTimeStamp("", value), "ParseTimeStamp of an empty string");
	Check(!viptest::ParseTimeStamp("abc", value), "ParseTimeStamp of letters");
	Check(!viptest::ParseTimeStamp("   ", value), "ParseTimeStamp of whitespace only");
	Check(!viptest::ParseTimeStamp("999999999999999999999999", value), "ParseTimeStamp of an overflow");

	// FormatLocalTime: the stored expiry in the server's local zone, and nullopt when
	// std::localtime cannot represent it (the caller prints the raw number then).
	const std::optional<std::string> formatted = viptest::FormatLocalTime(1700000000);
	Check(formatted.has_value(), "FormatLocalTime of a normal timestamp");
	if (formatted) {
		CheckEqInt(static_cast<long long>(formatted->size()), 19, "FormatLocalTime is %Y-%m-%d %H:%M:%S");
		const std::time_t raw = static_cast<std::time_t>(1700000000);
		char buffer[80] = {0};
		if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&raw)) != 0) {
			CheckEq(*formatted, buffer, "FormatLocalTime matches strftime");
		}
	}
	Check(!viptest::FormatLocalTime(std::numeric_limits<long long>::max()).has_value(),
		  "an out-of-range timestamp has no local time");
}

void TestCookieToString() {
	// The core's conversion of the stored cookie back to text; a string cookie is what this module
	// writes, the numeric/bool forms are what another writer (or an old core) could have stored.
	CheckEq(viptest::CookieToString(plg::any(plg::string("1700000000"))), "1700000000", "a string cookie");
	CheckEq(viptest::CookieToString(plg::any(true)), "1", "a bool cookie");
	CheckEq(viptest::CookieToString(plg::any(false)), "0", "a false cookie");
	CheckEq(viptest::CookieToString(plg::any(int64_t{5})), "5", "an int64 cookie");
	CheckEq(viptest::CookieToString(plg::any(int32_t{6})), "6", "an int32 cookie");
	CheckEq(viptest::CookieToString(plg::any(uint64_t{7})), "7", "a uint64 cookie");
	CheckEq(viptest::CookieToString(plg::any(uint32_t{8})), "8", "a uint32 cookie");
	CheckEq(viptest::CookieToString(plg::any(9.5)), "9.500000", "a double cookie");
	CheckEq(viptest::CookieToString(plg::any(1.5f)), "1.500000", "a float cookie");
	CheckEq(viptest::CookieToString(plg::any{}), "", "an empty cookie");
}

void TestConfig() {
	// A throwaway configs dir.
	std::error_code ec;
	const std::filesystem::path root = std::filesystem::temp_directory_path(ec) / "vip_test_tests";
	if (ec) {
		Check(false, "std::filesystem::temp_directory_path");
		return;
	}
	std::filesystem::remove_all(root, ec);
	std::filesystem::create_directories(root / "vip", ec);
	g_configsDir = root.string();

	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::Log = &FakeLog;

	CheckEq(viptest::ConfigPath("vip/vip_test.ini"), root.string() + "/vip/vip_test.ini", "ConfigPath joins");
	CheckEq(viptest::ConfigPath(""), root.string() + "/", "ConfigPath of an empty relative path");

	// Neither layout present: the production defaults with one error line.
	g_logLines.clear();
	{
		const viptest::VipTestConfig missing = viptest::LoadVipTestConfig();
		CheckEq(missing.group, viptest::kDefaultGroup, "a missing config keeps the default group");
		CheckEqInt(missing.time, viptest::kDefaultTime, "a missing config keeps the default time");
		CheckEqInt(missing.timeout, viptest::kDefaultTimeout, "a missing config keeps the default timeout");
		CheckEqInt(static_cast<long long>(g_logLines.size()), 1, "a missing config logs one error line");
	}

	// The production file in the shared vip/ directory (the Metamod location).
	{
		std::ofstream file(root / "vip" / "vip_test.ini", std::ios::binary | std::ios::trunc);
		file << kIniProduction;
	}
	g_logLines.clear();
	{
		const viptest::VipTestConfig loaded = viptest::LoadVipTestConfig();
		CheckEq(loaded.group, "VIPTEST", "the vip/ file is read: group");
		CheckEqInt(loaded.time, 1800, "the vip/ file is read: time");
		CheckEqInt(loaded.timeout, 172800, "the vip/ file is read: timeout");
		Check(g_logLines.empty(), "a valid vip/ file logs nothing");
	}

	// Custom values of the same file.
	{
		std::ofstream file(root / "vip" / "vip_test.ini", std::ios::binary | std::ios::trunc);
		file << "\"VIP\" { \"group\" \"GOLD\" \"time\" \"60\" \"timeout\" \"0\" }\n";
	}
	{
		const viptest::VipTestConfig custom = viptest::LoadVipTestConfig();
		CheckEq(custom.group, "GOLD", "a custom group is read");
		CheckEqInt(custom.time, 60, "a custom time is read");
		CheckEqInt(custom.timeout, 0, "a zero timeout is read");
	}
	// Missing keys keep the per-field defaults.
	{
		std::ofstream file(root / "vip" / "vip_test.ini", std::ios::binary | std::ios::trunc);
		file << "\"VIP\" { \"group\" \"GOLD\" }\n";
	}
	{
		const viptest::VipTestConfig partial = viptest::LoadVipTestConfig();
		CheckEq(partial.group, "GOLD", "a partial file keeps the group");
		CheckEqInt(partial.time, viptest::kDefaultTime, "a partial file keeps the default time");
		CheckEqInt(partial.timeout, viptest::kDefaultTimeout, "a partial file keeps the default timeout");
	}

	// The PLUGIN_STANDARD layout is the fallback when the shared vip/ file is absent.
	std::filesystem::remove(root / "vip" / "vip_test.ini", ec);
	std::filesystem::create_directories(root / "vip_test", ec);
	{
		std::ofstream file(root / "vip_test" / "vip_test.ini", std::ios::binary | std::ios::trunc);
		file << "\"VIP\" { \"group\" \"FALLBACK\" \"time\" \"120\" \"timeout\" \"3600\" }\n";
	}
	g_logLines.clear();
	{
		const viptest::VipTestConfig fallback = viptest::LoadVipTestConfig();
		CheckEq(fallback.group, "FALLBACK", "the vip_test/ fallback is read: group");
		CheckEqInt(fallback.time, 120, "the vip_test/ fallback is read: time");
		CheckEqInt(fallback.timeout, 3600, "the vip_test/ fallback is read: timeout");
		Check(g_logLines.empty(), "a valid fallback logs nothing");
	}

	// A malformed vip/ file falls through to the fallback layout.
	{
		std::ofstream file(root / "vip" / "vip_test.ini", std::ios::binary | std::ios::trunc);
		file << "\"VIP\" { \"group\" \"BROKEN\"\n";
	}
	{
		const viptest::VipTestConfig fallback = viptest::LoadVipTestConfig();
		CheckEq(fallback.group, "FALLBACK", "a malformed vip/ file falls through to the fallback");
	}
	// Both malformed: the production defaults and one error line.
	{
		std::ofstream file(root / "vip_test" / "vip_test.ini", std::ios::binary | std::ios::trunc);
		file << "\"VIP\" { \"group\" \"BROKEN\"\n";
	}
	g_logLines.clear();
	{
		const viptest::VipTestConfig broken = viptest::LoadVipTestConfig();
		CheckEq(broken.group, viptest::kDefaultGroup, "a broken pair keeps the default group");
		CheckEqInt(broken.time, viptest::kDefaultTime, "a broken pair keeps the default time");
		CheckEqInt(broken.timeout, viptest::kDefaultTimeout, "a broken pair keeps the default timeout");
		CheckEqInt(static_cast<long long>(g_logLines.size()), 1, "a broken pair logs one error line");
	}

	// No Plugify host (the configs dir is unavailable).
	plg::GetConfigsDir = nullptr;
	Check(viptest::ConfigPath("vip/vip_test.ini").empty(), "ConfigPath without a host is empty");
	g_logLines.clear();
	{
		const viptest::VipTestConfig noHost = viptest::LoadVipTestConfig();
		CheckEq(noHost.group, viptest::kDefaultGroup, "no configs dir keeps the default group");
		CheckEqInt(noHost.time, viptest::kDefaultTime, "no configs dir keeps the default time");
		CheckEqInt(noHost.timeout, viptest::kDefaultTimeout, "no configs dir keeps the default timeout");
		CheckEqInt(static_cast<long long>(g_logLines.size()), 1, "no configs dir logs one error line");
	}

	// A trailing slash in the host answer is not doubled.
	plg::GetConfigsDir = &FakeGetConfigsDir;
	g_configsDir = root.string() + "/";
	CheckEq(viptest::ConfigPath("x.ini"), root.string() + "/x.ini", "ConfigPath handles a trailing slash");

	std::filesystem::remove_all(root, ec);
	g_configsDir.clear();
}

// ==============================================================================================
// Part 2: the guards and the command dispatch with fake s2sdk bindings
// ==============================================================================================

struct FakeClient {
	bool connected = false;
	bool inGame = false;
	bool sourceTV = false;
	bool fake = false;
	uint64_t steamid = 0;
};

std::vector<FakeClient> g_clients(static_cast<std::size_t>(viptest::kMaxSlots));
FakeClient g_unexpectedClient; // receives an out-of-range slot so a bug cannot corrupt memory
int g_outOfRangeCalls = 0;

FakeClient& At(int32_t slot) {
	if (slot < 0 || slot >= viptest::kMaxSlots) {
		++g_outOfRangeCalls;
		return g_unexpectedClient;
	}
	return g_clients[static_cast<std::size_t>(slot)];
}

constexpr uint64_t kSteamId = 76561198000000001ULL;

int g_connectedCalls = 0;
int g_inGameCalls = 0;
int g_sourceTVCalls = 0;
int g_fakeCalls = 0;
int g_steamIdCalls = 0;

void ResetClientCounters() {
	g_connectedCalls = 0;
	g_inGameCalls = 0;
	g_sourceTVCalls = 0;
	g_fakeCalls = 0;
	g_steamIdCalls = 0;
	g_outOfRangeCalls = 0;
}

void ResetClients() {
	std::fill(g_clients.begin(), g_clients.end(), FakeClient{});
	g_unexpectedClient = FakeClient{};
}

FakeClient& ConnectClient(int32_t slot, uint64_t steamid = kSteamId) {
	FakeClient& client = g_clients[static_cast<std::size_t>(slot)];
	client = FakeClient{};
	client.connected = true;
	client.inGame = true;
	client.steamid = steamid;
	return client;
}

bool FakeIsClientConnected(int32_t slot) {
	++g_connectedCalls;
	return At(slot).connected;
}

bool FakeIsClientInGame(int32_t slot) {
	++g_inGameCalls;
	return At(slot).inGame;
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_sourceTVCalls;
	return At(slot).sourceTV;
}

bool FakeIsFakeClient(int32_t slot) {
	++g_fakeCalls;
	return At(slot).fake;
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	++g_steamIdCalls;
	return At(slot).steamid;
}

struct FakePrint {
	int32_t slot = 0;
	std::string text;
};

std::vector<FakePrint> g_chatPrints;
std::vector<std::string> g_serverPrints;

void FakePrintToChat(int32_t slot, const plg::string& message) {
	g_chatPrints.push_back({slot, ToStd(message)});
}

void FakePrintToServer(const plg::string& message) {
	g_serverPrints.push_back(ToStd(message));
}

// --- map listeners ----------------------------------------------------------------------------

s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
int g_mapStartRegistrations = 0;
int g_mapStartUnregistrations = 0;
int g_mapEndRegistrations = 0;
int g_mapEndUnregistrations = 0;

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
	++g_mapStartRegistrations;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_mapStartCallback == callback) {
		g_mapStartCallback = nullptr;
	}
	++g_mapStartUnregistrations;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapEndCallback = callback;
	++g_mapEndRegistrations;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_mapEndCallback == callback) {
		g_mapEndCallback = nullptr;
	}
	++g_mapEndUnregistrations;
}

void FireMapStart() {
	if (g_mapStartCallback != nullptr) {
		g_mapStartCallback();
	}
}

void FireMapEnd() {
	if (g_mapEndCallback != nullptr) {
		g_mapEndCallback();
	}
}

// --- command bindings -------------------------------------------------------------------------

s2sdk::ConCommandCallback g_commandCallback = nullptr;
s2sdk::ConVarFlag g_lastCommandFlags = s2sdk::ConVarFlag::None;
std::vector<std::string> g_commandNames;
std::vector<std::string> g_existingCommands; // AddConsoleCommand answers false for these names
std::vector<std::pair<std::string, s2sdk::ConCommandCallback>> g_listeners;
int g_addedCommands = 0;
int g_removedCommands = 0;
int g_removedListeners = 0;

bool FakeAddConsoleCommand(const plg::string& name, const plg::string& /*description*/,
						   s2sdk::ConVarFlag flags, s2sdk::ConCommandCallback callback,
						   s2sdk::HookMode /*type*/) {
	++g_addedCommands;
	g_commandCallback = callback;
	g_lastCommandFlags = flags;
	const std::string text = ToStd(name);
	g_commandNames.push_back(text);
	return std::find(g_existingCommands.begin(), g_existingCommands.end(), text) == g_existingCommands.end();
}

bool FakeRemoveCommand(const plg::string& /*name*/, s2sdk::ConCommandCallback /*callback*/) {
	++g_removedCommands;
	return true;
}

bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback callback,
							s2sdk::HookMode /*type*/) {
	g_listeners.emplace_back(ToStd(name), callback);
	return true;
}

bool FakeRemoveCommandListener(const plg::string& /*name*/, s2sdk::ConCommandCallback /*callback*/,
							   s2sdk::HookMode /*type*/) {
	++g_removedListeners;
	return true;
}

// --- vip bindings -----------------------------------------------------------------------------

vip::ReadyDelegate g_readyDelegate = nullptr;
int g_readyRegistrations = 0;
int g_readyUnregistrations = 0;
bool g_vipLoaded = false;
bool g_clientIsVip = false;
std::string g_vipGroup;
int g_vipIsClientVipCalls = 0;

int g_giveCalls = 0;
int32_t g_lastGiveSlot = -99;
int32_t g_lastGiveTime = -1;
std::string g_lastGiveGroup;
bool g_lastGiveAddToDB = false;

constexpr char kAlreadyVipPhrase[] = "You are {GREEN}VIP {DEFAULT}already!";
constexpr char kAlreadyReceivedPhrase[] =
		"Re-acquisition of {GREEN}VIP status {DEFAULT} will be available %s!";
constexpr char kReceivedDisabledPhrase[] = "Re-acquisition of {GREEN}VIP status {RED} is disabled!";

void FakeVipOnVIPLoadedRegister(vip::ReadyDelegate callback) {
	g_readyDelegate = callback;
	++g_readyRegistrations;
}

void FakeVipOnVIPLoadedUnregister(vip::ReadyDelegate callback) {
	if (g_readyDelegate == callback) {
		g_readyDelegate = nullptr;
	}
	++g_readyUnregistrations;
}

bool FakeVipIsVIPLoaded() {
	return g_vipLoaded;
}

plg::string FakeVipGetTranslate(const plg::string& phrase) {
	const std::string key = ToStd(phrase);
	if (key == "AlreadyVIP") {
		return ToPlg(kAlreadyVipPhrase);
	}
	if (key == "AlreadyReceived") {
		return ToPlg(kAlreadyReceivedPhrase);
	}
	if (key == "ReceivedDisabled") {
		return ToPlg(kReceivedDisabledPhrase);
	}
	return {};
}

bool FakeVipIsClientVIP(int32_t /*slot*/) {
	++g_vipIsClientVipCalls;
	return g_clientIsVip;
}

plg::string FakeVipGetClientVIPGroup(int32_t /*slot*/) {
	return ToPlg(g_vipGroup);
}

bool FakeVipGiveClientVIP(int32_t slot, int32_t time, const plg::string& group, bool addToDB) {
	++g_giveCalls;
	g_lastGiveSlot = slot;
	g_lastGiveTime = time;
	g_lastGiveGroup = ToStd(group);
	g_lastGiveAddToDB = addToDB;
	return true;
}

// --- permissions cookie bindings --------------------------------------------------------------

std::map<std::string, std::string> g_cookieValues;
std::vector<std::string> g_cookieNames; // every SetCookie key, in order
int g_getCookieCalls = 0;
int g_setCookieCalls = 0;
int64_t g_lastSetCookiePlugin = -1;
uint64_t g_lastSetCookieTarget = 0;
std::string g_lastSetCookieName;

permissions::Status FakeGetCookie(uint64_t targetID, const plg::string& name, plg::any& value) {
	++g_getCookieCalls;
	(void)targetID;
	const std::string key = ToStd(name);
	const auto it = g_cookieValues.find(key);
	if (it == g_cookieValues.end()) {
		return permissions::Status::CookieNotFound;
	}
	value = ToPlg(it->second);
	return permissions::Status::Success;
}

permissions::Status FakeSetCookie(int64_t pluginID, uint64_t targetID, const plg::string& name,
								  const plg::any& cookie, bool /*dontBroadcast*/) {
	++g_setCookieCalls;
	g_lastSetCookiePlugin = pluginID;
	g_lastSetCookieTarget = targetID;
	g_lastSetCookieName = ToStd(name);
	g_cookieNames.push_back(g_lastSetCookieName);
	// This module always writes a plg::string; anything else would be a bug worth failing on.
	if (plg::holds_alternative<plg::string>(cookie)) {
		g_cookieValues[g_lastSetCookieName] = ToStd(plg::get<plg::string>(cookie));
	}
	return permissions::Status::Success;
}

// --- installing and clearing the fakes --------------------------------------------------------

void InstallFakes() {
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintToServer = &FakePrintToServer;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;

	__vip_VIP_IsVIPLoaded = &FakeVipIsVIPLoaded;
	__vip_VIP_OnVIPLoaded_Register = &FakeVipOnVIPLoadedRegister;
	__vip_VIP_OnVIPLoaded_Unregister = &FakeVipOnVIPLoadedUnregister;
	__vip_VIP_GetTranslate = &FakeVipGetTranslate;
	__vip_VIP_IsClientVIP = &FakeVipIsClientVIP;
	__vip_VIP_GetClientVIPGroup = &FakeVipGetClientVIPGroup;
	__vip_VIP_GiveClientVIP = &FakeVipGiveClientVIP;

	__permissions_GetCookie = &FakeGetCookie;
	__permissions_SetCookie = &FakeSetCookie;

	plg::GetConfigsDir = &FakeGetConfigsDir;
	plg::Log = &FakeLog;
	plg::plugin::GetId = &FakeGetId;
}

// The vip core is an optional dependency: a host that did not fill its binding slots must leave the
// plugin harmless (InstallFakes() restores every slot afterwards).
void ClearVipBindings() {
	__vip_VIP_IsVIPLoaded = nullptr;
	__vip_VIP_OnVIPLoaded_Register = nullptr;
	__vip_VIP_OnVIPLoaded_Unregister = nullptr;
	__vip_VIP_GetTranslate = nullptr;
	__vip_VIP_IsClientVIP = nullptr;
	__vip_VIP_GetClientVIPGroup = nullptr;
	__vip_VIP_GiveClientVIP = nullptr;
}

void TestGuards() {
	ResetClients();
	ResetClientCounters();

	// 1. Without the map listeners the gate must stay shut instead of guessing readiness.
	__s2sdk_OnMapStart_Register = nullptr;
	__s2sdk_OnMapEnd_Register = nullptr;
	g_logLines.clear();
	viptest::InitMapGate();
	viptest::PollMapGate();
	Check(!viptest::IsMapReady(), "without the map listeners the gate stays shut");
	Check(!g_logLines.empty(), "without the map listeners one error line is logged");
	Check(!viptest::HasClient(0), "without the map listeners HasClient is false");
	CheckEqInt(g_connectedCalls, 0, "without the map listeners no client query is made");

	// 2. Bindings available: OnPluginStart registers both listeners exactly once.
	InstallFakes();
	g_mapStartRegistrations = 0;
	g_mapEndRegistrations = 0;
	viptest::InitMapGate();
	CheckEqInt(g_mapStartRegistrations, 1, "OnMapStart listener registered once");
	CheckEqInt(g_mapEndRegistrations, 1, "OnMapEnd listener registered once");
	Check(!viptest::IsMapReady(), "the gate is shut until a map start or the first update");

	// 3. A plugin loaded into a running map: the first update proves a live map.
	viptest::PollMapGate();
	Check(viptest::IsMapReady(), "the first update after OnPluginStart opens the gate");

	// 4. Out-of-range slots: no s2sdk call at all.
	ResetClientCounters();
	Check(!viptest::HasClient(-1), "slot -1 is refused");
	Check(!viptest::HasClient(viptest::kMaxSlots), "slot 64 is refused");
	CheckEqInt(g_connectedCalls, 0, "no client query for an out-of-range slot");
	CheckEqInt(viptest::ClientSteamID64(-1), 0, "an out-of-range slot has no SteamID");
	CheckEqInt(g_steamIdCalls, 0, "GetClientSteamID64 is not called for an out-of-range slot");

	// 5. The layers of HasClient(), each checked before the next export is called.
	ConnectClient(1);
	Check(viptest::HasClient(1), "a connected, in-game, non-SourceTV, non-bot client passes");

	ResetClientCounters();
	Check(!viptest::HasClient(2), "an empty slot has no client");
	CheckEqInt(g_connectedCalls, 1, "an empty slot costs one IsClientConnected");
	CheckEqInt(g_inGameCalls, 0, "an empty slot never reaches IsClientInGame");

	ResetClientCounters();
	g_clients[3].connected = true; // connected, not in game
	Check(!viptest::HasClient(3), "a client that is not in game is refused");
	CheckEqInt(g_inGameCalls, 1, "a client that is not in game reaches IsClientInGame");
	CheckEqInt(g_sourceTVCalls, 0, "a client that is not in game never reaches IsClientSourceTV");

	ResetClientCounters();
	ConnectClient(4);
	g_clients[4].sourceTV = true;
	Check(!viptest::HasClient(4), "SourceTV is refused");
	CheckEqInt(g_sourceTVCalls, 1, "SourceTV reaches IsClientSourceTV");
	CheckEqInt(g_fakeCalls, 0, "SourceTV never reaches IsFakeClient");

	ResetClientCounters();
	ConnectClient(5);
	g_clients[5].fake = true;
	Check(!viptest::HasClient(5), "a bot is refused");
	CheckEqInt(g_fakeCalls, 1, "a bot reaches IsFakeClient");

	// A missing binding refuses the client instead of calling a null pointer.
	ResetClientCounters();
	const s2sdk::_IsFakeClient savedFake = __s2sdk_IsFakeClient;
	__s2sdk_IsFakeClient = nullptr;
	Check(!viptest::HasClient(1), "a missing IsFakeClient binding refuses the client");
	CheckEqInt(g_fakeCalls, 0, "a missing IsFakeClient binding is never called");
	__s2sdk_IsFakeClient = savedFake;

	// 6. ClientSteamID64 only for an accepted client.
	ResetClientCounters();
	CheckEqInt(static_cast<long long>(viptest::ClientSteamID64(1)), static_cast<long long>(kSteamId),
			   "ClientSteamID64 of a live client");
	CheckEqInt(g_steamIdCalls, 1, "GetClientSteamID64 is called once");
	ResetClientCounters();
	CheckEqInt(viptest::ClientSteamID64(2), 0, "an empty slot has no SteamID");
	CheckEqInt(g_steamIdCalls, 0, "an empty slot never reaches GetClientSteamID64");
	const s2sdk::_GetClientSteamID64 savedSteamId = __s2sdk_GetClientSteamID64;
	__s2sdk_GetClientSteamID64 = nullptr;
	CheckEqInt(viptest::ClientSteamID64(1), 0, "a missing GetClientSteamID64 binding answers 0");
	__s2sdk_GetClientSteamID64 = savedSteamId;

	// 7. PrintToChat: behind HasClient(), colourised; the server console for a negative slot.
	g_chatPrints.clear();
	g_serverPrints.clear();
	viptest::PrintToChat(1, "{GREEN}hi{DEFAULT}");
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 1, "an in-game client gets the chat line");
	Check(g_chatPrints.size() == 1 && g_chatPrints[0].slot == 1, "the chat line goes to the caller slot");
	Check(g_chatPrints.size() == 1 && g_chatPrints[0].text == "\x04" "hi" "\x01", "the chat line is colorized");
	viptest::PrintToChat(2, "{GREEN}hi{DEFAULT}");
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 1, "an empty slot gets nothing");
	g_serverPrints.clear();
	viptest::PrintToChat(-1, "{GREEN}hi{DEFAULT}");
	CheckEqInt(static_cast<long long>(g_serverPrints.size()), 1, "a negative slot prints to the server");
	Check(g_serverPrints.size() == 1 && g_serverPrints[0] == "hi", "the server line has its tags stripped");
	viptest::PrintToChat(-1, "line\\n");
	Check(g_serverPrints.size() == 2 && g_serverPrints[1] == "line",
		  "the server line drops the trailing chat line break");

	// 8. OnMapEnd: closed gate, and not one client query afterwards (PITFALLS B2).
	FireMapEnd();
	Check(!viptest::IsMapReady(), "OnMapEnd closes the gate");
	ResetClientCounters();
	Check(!viptest::HasClient(1), "no client after OnMapEnd");
	CheckEqInt(viptest::ClientSteamID64(1), 0, "no SteamID after OnMapEnd");
	CheckEqInt(g_connectedCalls, 0, "no s2sdk client query in the map-change window");
	CheckEqInt(g_steamIdCalls, 0, "no GetClientSteamID64 in the map-change window");
	g_chatPrints.clear();
	viptest::PrintToChat(1, "{GREEN}hi");
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 0, "no chat line in the map-change window");

	// The host keeps pumping updates through a changelevel: they must not reopen the gate.
	viptest::PollMapGate();
	Check(!viptest::IsMapReady(), "updates alone do not reopen the gate after OnMapEnd");

	// The next OnMapStart reopens it.
	FireMapStart();
	Check(viptest::IsMapReady(), "OnMapStart reopens the gate");
	Check(viptest::HasClient(1), "the client passes HasClient on the next map");

	// 9. Unload: the listeners are removed and a second shutdown is a no-op (PITFALLS A7).
	g_mapStartUnregistrations = 0;
	g_mapEndUnregistrations = 0;
	viptest::ShutdownMapGate();
	CheckEqInt(g_mapStartUnregistrations, 1, "the OnMapStart listener is unregistered once");
	CheckEqInt(g_mapEndUnregistrations, 1, "the OnMapEnd listener is unregistered once");
	Check(g_mapStartCallback == nullptr && g_mapEndCallback == nullptr, "both listeners are detached");
	Check(!viptest::IsMapReady(), "the gate is shut after shutdown");
	viptest::ShutdownMapGate();
	CheckEqInt(g_mapStartUnregistrations, 1, "ShutdownMapGate is idempotent");

	CheckEqInt(g_outOfRangeCalls, 0, "no s2sdk export was called with an out-of-range slot");
}

int g_handlerCalls = 0;
int32_t g_lastCaller = -99;

s2sdk::ResultType CountingHandler(int32_t slot) {
	++g_handlerCalls;
	g_lastCaller = slot;
	return s2sdk::ResultType::Handled;
}

plg::vector<plg::string> Args(std::initializer_list<std::string_view> words) {
	plg::vector<plg::string> out;
	for (const std::string_view word : words) {
		out.push_back(ToPlg(std::string(word)));
	}
	return out;
}

void TestCommands() {
	g_existingCommands = {"mm_testvip"}; // the engine/another plugin already owns this name
	g_addedCommands = 0;
	g_removedCommands = 0;
	g_removedListeners = 0;
	g_commandNames.clear();
	g_listeners.clear();
	g_handlerCalls = 0;

	// The original's registrations: four console names, two chat names, two bare words.
	Check(viptest::AddCommand("mm_viptest", viptest::CommandFrom::Console, &CountingHandler, ""),
		  "mm_viptest registers for the console");
	Check(viptest::AddCommand("sm_viptest", viptest::CommandFrom::Console, &CountingHandler, ""),
		  "sm_viptest registers for the console");
	Check(viptest::AddCommand("mm_testvip", viptest::CommandFrom::Console, &CountingHandler, ""),
		  "mm_testvip registers for the console (an existing name is reused)");
	Check(viptest::AddCommand("sm_testvip", viptest::CommandFrom::Console, &CountingHandler, ""),
		  "sm_testvip registers for the console");
	Check(viptest::AddCommand("viptest", viptest::CommandFrom::Chat, &CountingHandler, ""),
		  "viptest registers for chat");
	Check(viptest::AddCommand("testvip", viptest::CommandFrom::Chat, &CountingHandler, ""),
		  "testvip registers for chat");
	Check(viptest::AddChatWord("viptest", &CountingHandler), "the bare word viptest registers");
	Check(viptest::AddChatWord("testvip", &CountingHandler), "the bare word testvip registers");

	CheckEqInt(g_addedCommands, 6, "six console names go through AddConsoleCommand");
	CheckEqInt(static_cast<long long>(g_listeners.size()), 2, "say and say_team listeners installed");
	Check(g_listeners.size() == 2 && g_listeners[0].first == "say" && g_listeners[1].first == "say_team",
		  "the listeners are on say and say_team");
	// PITFALLS C11: a client console command must be client-executable.
	Check((static_cast<int64_t>(g_lastCommandFlags) &
		   static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute)) != 0,
		  "the commands are ClientCanExecute");
	Check((static_cast<int64_t>(g_lastCommandFlags) & static_cast<int64_t>(s2sdk::ConVarFlag::Release)) != 0,
		  "the commands are Release");

	// Duplicate registrations and bad names are refused.
	Check(!viptest::AddCommand("viptest", viptest::CommandFrom::Chat, &CountingHandler, ""),
		  "a duplicate command source is refused");
	Check(!viptest::AddChatWord("viptest", &CountingHandler), "a duplicate chat word is refused");
	Check(!viptest::AddCommand("!viptest", viptest::CommandFrom::Both, &CountingHandler, ""),
		  "a name with a chat trigger is refused");
	Check(!viptest::AddCommand("", viptest::CommandFrom::Both, &CountingHandler, ""), "an empty name is refused");
	Check(!viptest::AddCommand("viptest2", viptest::CommandFrom::Console, nullptr, ""),
		  "a null handler is refused");

	Check(g_commandCallback != nullptr, "the command trampoline was captured");
	if (g_commandCallback == nullptr) {
		return;
	}
	const s2sdk::ConCommandCallback command = g_commandCallback;

	// The console names answer in a console (the server one included) ...
	g_handlerCalls = 0;
	g_lastCaller = -99;
	Check(command(-1, s2sdk::ConCommandContext::Console, Args({"mm_viptest"})) == s2sdk::ResultType::Handled,
		  "mm_viptest answers in the console with Handled");
	CheckEqInt(g_handlerCalls, 1, "the mm_viptest handler ran once");
	CheckEqInt(g_lastCaller, -1, "the server console caller is forwarded");
	Check(command(5, s2sdk::ConCommandContext::Console, Args({"sm_viptest"})) == s2sdk::ResultType::Handled,
		  "sm_viptest answers in a client console");
	CheckEqInt(g_lastCaller, 5, "a client console caller is forwarded");
	Check(g_commandNames.size() == 6 && g_commandNames[0] == "mm_viptest" && g_commandNames[1] == "sm_viptest" &&
				  g_commandNames[2] == "mm_testvip" && g_commandNames[3] == "sm_testvip" &&
				  g_commandNames[4] == "viptest" && g_commandNames[5] == "testvip",
		  "the console names are registered in order");

	// ... and are left to the engine in chat ("!mm_viptest" must not answer).
	g_handlerCalls = 0;
	Check(command(5, s2sdk::ConCommandContext::Chat, Args({"mm_viptest"})) == s2sdk::ResultType::Continue,
		  "console-only mm_viptest is left to the engine in chat");
	Check(command(5, s2sdk::ConCommandContext::Chat, Args({"sm_viptest"})) == s2sdk::ResultType::Continue,
		  "console-only sm_viptest is left to the engine in chat");
	Check(command(5, s2sdk::ConCommandContext::Chat, Args({"mm_testvip"})) == s2sdk::ResultType::Continue,
		  "console-only mm_testvip is left to the engine in chat");
	Check(command(5, s2sdk::ConCommandContext::Chat, Args({"sm_testvip"})) == s2sdk::ResultType::Continue,
		  "console-only sm_testvip is left to the engine in chat");
	CheckEqInt(g_handlerCalls, 0, "the console-only names never ran in chat");

	// The chat names answer "!viptest" / "/testvip" and not a console spelling.
	g_handlerCalls = 0;
	g_lastCaller = -99;
	Check(command(6, s2sdk::ConCommandContext::Chat, Args({"viptest"})) == s2sdk::ResultType::Handled,
		  "viptest answers in chat");
	CheckEqInt(g_lastCaller, 6, "the chat caller is forwarded");
	Check(command(6, s2sdk::ConCommandContext::Chat, Args({"testvip"})) == s2sdk::ResultType::Handled,
		  "testvip answers in chat");
	Check(command(-1, s2sdk::ConCommandContext::Console, Args({"viptest"})) == s2sdk::ResultType::Continue,
		  "viptest is not a console command");
	Check(command(6, s2sdk::ConCommandContext::Console, Args({"testvip"})) == s2sdk::ResultType::Continue,
		  "testvip is not a console command");
	CheckEqInt(g_handlerCalls, 2, "the console spellings of the chat names never ran");

	// An unknown command is left to the engine; empty args are safe.
	Check(command(6, s2sdk::ConCommandContext::Chat, Args({"nosuchcommand"})) == s2sdk::ResultType::Continue,
		  "an unknown command is Continue");
	Check(command(6, s2sdk::ConCommandContext::Chat, plg::vector<plg::string>{}) == s2sdk::ResultType::Continue,
		  "empty command args are Continue");

	// The bare words through the say/say_team listeners.
	const s2sdk::ConCommandCallback say = g_listeners[0].second;
	const s2sdk::ConCommandCallback sayTeam = g_listeners[1].second;
	g_handlerCalls = 0;
	g_lastCaller = -99;
	Check(say(7, s2sdk::ConCommandContext::Chat, Args({"say", "viptest"})) == s2sdk::ResultType::Handled,
		  "say viptest answers with Handled");
	CheckEqInt(g_handlerCalls, 1, "the bare word ran the handler");
	CheckEqInt(g_lastCaller, 7, "the say caller is forwarded");
	Check(sayTeam(8, s2sdk::ConCommandContext::Chat, Args({"say_team", "TESTVIP"})) ==
				  s2sdk::ResultType::Handled,
		  "say_team TESTVIP answers case-insensitively");
	CheckEqInt(g_lastCaller, 8, "the say_team caller is forwarded");
	Check(say(-1, s2sdk::ConCommandContext::Chat, Args({"say", "testvip", "top"})) ==
				  s2sdk::ResultType::Handled,
		  "a console-style say with several words still matches the first word");

	g_handlerCalls = 0;
	Check(say(7, s2sdk::ConCommandContext::Chat, Args({"say", "viptest top", "x"})) ==
				  s2sdk::ResultType::Continue,
		  "an already-tokenised quoted first argument is not split again");
	Check(say(7, s2sdk::ConCommandContext::Chat, Args({"say", "!viptest"})) == s2sdk::ResultType::Continue,
		  "say !viptest is left to the chat command (no double handling)");
	Check(say(7, s2sdk::ConCommandContext::Chat, Args({"say", "top viptest"})) == s2sdk::ResultType::Continue,
		  "a later word does not trigger the word");
	Check(say(7, s2sdk::ConCommandContext::Chat, Args({"say"})) == s2sdk::ResultType::Continue,
		  "a say line without a message is Continue");
	CheckEqInt(g_handlerCalls, 0, "the non-matching lines never ran the handler");

	// OnPluginEnd: every name and listener is detached (PITFALLS A7). "mm_testvip" already existed,
	// so only its listener may be removed - never RemoveCommand on someone else's command.
	viptest::RemoveCommands();
	CheckEqInt(g_removedCommands, 5, "the five owned commands are removed with RemoveCommand");
	CheckEqInt(g_removedListeners, 3, "mm_testvip and the say/say_team listeners are detached");
	viptest::RemoveCommands();
	CheckEqInt(g_removedCommands, 5, "RemoveCommands is idempotent (commands)");
	CheckEqInt(g_removedListeners, 3, "RemoveCommands is idempotent (listeners)");

	// A reload registers the names again from scratch.
	g_addedCommands = 0;
	g_commandNames.clear();
	g_listeners.clear();
	Check(viptest::AddCommand("viptest", viptest::CommandFrom::Chat, &CountingHandler, ""),
		  "a reload registers the command again");
	CheckEqInt(g_addedCommands, 1, "a reload calls AddConsoleCommand again");
	CheckEqInt(static_cast<long long>(g_listeners.size()), 0, "no say listener without a chat word");
	viptest::RemoveCommands();
	CheckEqInt(g_addedCommands, 1, "the reloaded command was not registered twice");
}

// ==============================================================================================
// Part 3: the plugin TU — lifecycle and the handler flow
// ==============================================================================================

// A start without the vip core (the optional dependency absent) must never fail and must never
// call a binding that the host did not fill (PITFALLS A1).
void TestPluginStartWithoutVipBindings() {
	ResetClients();
	ResetClientCounters();
	InstallFakes();
	ClearVipBindings();
	g_configsDir = VIP_TEST_TEST_DATA_DIR;
	g_logLines.clear();
	g_readyRegistrations = 0;
	g_mapStartRegistrations = 0;
	g_mapEndRegistrations = 0;
	g_chatPrints.clear();
	g_serverPrints.clear();

	Check(g_plugin.OnPluginStart().GetCode() == plg::PluginCode::Ok,
		  "OnPluginStart never fails without the vip bindings");
	CheckEqInt(g_readyRegistrations, 0, "no VIP_OnVIPLoaded registration without the bindings");
	CheckEqInt(g_mapStartRegistrations, 1, "the map listeners are registered anyway");
	CheckEqInt(g_mapEndRegistrations, 1, "the OnMapEnd listener is registered anyway");
	CheckEq(g_config.group, "VIPTEST", "the config is loaded without the vip bindings");
	CheckEqInt(g_config.time, 1800, "the time is loaded without the vip bindings");
	CheckEqInt(g_config.timeout, 172800, "the timeout is loaded without the vip bindings");

	// The update pump still runs, and the handler is harmless (no vip call is possible).
	Check(g_plugin.OnPluginUpdate(std::chrono::milliseconds(0)).GetCode() == plg::PluginCode::Ok,
		  "OnPluginUpdate never fails without the vip bindings");
	Check(viptest::IsMapReady(), "the map gate opens without the vip bindings");
	ConnectClient(1);
	g_cookieValues.clear();
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled,
		  "the handler answers Handled without the vip bindings");
	CheckEqInt(g_giveCalls, 0, "no VIP is given without the vip bindings");

	// Unload and restore every binding slot for the next test.
	Check(g_plugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd never fails");
	InstallFakes();
}

void TestPluginFlow() {
	ResetClients();
	ResetClientCounters();
	InstallFakes();
	g_configsDir = VIP_TEST_TEST_DATA_DIR;
	g_logLines.clear();
	g_vipLoaded = false;
	g_clientIsVip = false;
	g_vipGroup.clear();
	g_cookieValues.clear();
	g_cookieNames.clear();
	g_getCookieCalls = 0;
	g_setCookieCalls = 0;
	g_giveCalls = 0;
	g_chatPrints.clear();
	g_serverPrints.clear();
	g_readyDelegate = nullptr;
	g_readyRegistrations = 0;
	g_readyUnregistrations = 0;
	g_addedCommands = 0;
	g_commandNames.clear();
	g_existingCommands.clear(); // a fresh server owns none of the vip_test names
	g_listeners.clear();
	g_mapStartRegistrations = 0;
	g_mapEndRegistrations = 0;
	g_mapStartUnregistrations = 0;
	g_mapEndUnregistrations = 0;

	// --- start: map listeners first, then the VIP_OnVIPLoaded wait ------------------------------
	Check(g_plugin.OnPluginStart().GetCode() == plg::PluginCode::Ok, "OnPluginStart succeeds");
	CheckEqInt(g_mapStartRegistrations, 1, "OnPluginStart registers the OnMapStart listener");
	CheckEqInt(g_mapEndRegistrations, 1, "OnPluginStart registers the OnMapEnd listener");
	CheckEqInt(g_readyRegistrations, 1, "OnPluginStart registers VIP_OnVIPLoaded");
	Check(g_readyDelegate != nullptr, "the ready callback was captured");
	CheckEq(g_config.group, "VIPTEST", "the production group is read");
	CheckEqInt(g_config.time, 1800, "the production time is read");
	CheckEqInt(g_config.timeout, 172800, "the production timeout is read");
	CheckEqInt(g_addedCommands, 0, "no command is registered before the vip core is ready");

	// The gate is shut until the first update (a plugin loaded into a running map).
	ResetClientCounters();
	Check(!viptest::HasClient(1), "the gate is shut right after OnPluginStart");
	CheckEqInt(g_connectedCalls, 0, "no client query right after OnPluginStart");

	Check(g_plugin.OnPluginUpdate(std::chrono::milliseconds(0)).GetCode() == plg::PluginCode::Ok,
		  "OnPluginUpdate succeeds");
	Check(viptest::IsMapReady(), "the first update opens the gate");
	ConnectClient(1);
	Check(viptest::HasClient(1), "the client passes HasClient on the live map");

	// --- the core becomes ready: the six commands and the two words appear ---------------------
	if (g_readyDelegate != nullptr) {
		g_readyDelegate();
	}
	CheckEqInt(g_addedCommands, 6, "the ready callback registers six console names");
	CheckEqInt(static_cast<long long>(g_listeners.size()), 2, "the ready callback installs say/say_team");
	Check(g_commandNames.size() == 6 && g_commandNames[0] == "mm_viptest" && g_commandNames[1] == "sm_viptest" &&
				  g_commandNames[2] == "mm_testvip" && g_commandNames[3] == "sm_testvip" &&
				  g_commandNames[4] == "viptest" && g_commandNames[5] == "testvip",
		  "the commands are the original's, in order");
	if (g_readyDelegate != nullptr) {
		g_readyDelegate(); // a repeated VIP_OnVIPLoaded must not register a second time
	}
	CheckEqInt(g_addedCommands, 6, "the ready callback registers the commands once");

	// --- first grant: no cookie yet ------------------------------------------------------------
	const std::time_t before = std::time(nullptr);
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled, "the first call answers Handled");
	const std::time_t after = std::time(nullptr);
	CheckEqInt(g_giveCalls, 1, "the first call gives the VIP period");
	Check(g_lastGiveSlot == 1 && g_lastGiveTime == 1800 && g_lastGiveGroup == "VIPTEST",
		  "VIP_GiveClientVIP(slot, 1800, VIPTEST)");
	Check(g_lastGiveAddToDB, "the grant is written to the database (the API default)");
	CheckEqInt(g_getCookieCalls, 1, "the cooldown cookie is read once");
	CheckEqInt(g_setCookieCalls, 1, "the cooldown cookie is written once");
	CheckEqInt(g_lastSetCookiePlugin, kPluginId, "the write carries this plugin's id");
	CheckEqInt(static_cast<long long>(g_lastSetCookieTarget), static_cast<long long>(kSteamId),
			   "the write targets the caller's SteamID64");
	CheckEq(g_lastSetCookieName, ".vip_test", "the key is '<group>.vip_test' with the empty group");
	Check(g_chatPrints.empty(), "a grant prints nothing to chat");
	Check(g_serverPrints.empty(), "a grant prints nothing to the server console");

	long long stored = 0;
	const auto key = g_cookieValues.find(".vip_test");
	Check(key != g_cookieValues.end(), "the cookie was stored");
	if (key != g_cookieValues.end()) {
		Check(viptest::ParseTimeStamp(key->second, stored), "the stored cookie is a timestamp");
		Check(stored >= static_cast<long long>(before) + 1800 + 172800 &&
					  stored <= static_cast<long long>(after) + 1800 + 172800,
			  "the stored value is now + time + timeout");
	}

	// --- second call: the cooldown reports and neither grants nor rewrites ---------------------
	g_giveCalls = 0;
	g_setCookieCalls = 0;
	g_chatPrints.clear();
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled, "the second call answers Handled");
	CheckEqInt(g_giveCalls, 0, "a running cooldown does not grant again");
	CheckEqInt(g_setCookieCalls, 0, "the cookie is not rewritten during the cooldown");
	CheckEqInt(static_cast<long long>(g_chatPrints.size()), 1, "the cooldown prints exactly one line");
	const std::optional<std::string> when = viptest::FormatLocalTime(stored);
	Check(when.has_value(), "the stored expiry is a printable local time");
	if (when && !g_chatPrints.empty()) {
		CheckEq(g_chatPrints[0].text, viptest::Colorize(viptest::FormatPhrase(kAlreadyReceivedPhrase, when->c_str())),
				"AlreadyReceived with the stored expiry, colourised");
		Check(g_chatPrints[0].slot == 1, "the report goes to the caller");
	}

	// --- timeout 0: a stored value never allows a second grant --------------------------------
	g_config.timeout = 0;
	g_giveCalls = 0;
	g_setCookieCalls = 0;
	g_chatPrints.clear();
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled, "timeout 0 answers Handled");
	CheckEqInt(g_giveCalls, 0, "timeout 0 never grants a stored value again");
	CheckEqInt(g_setCookieCalls, 0, "timeout 0 does not rewrite the cookie");
	Check(g_chatPrints.size() == 1 && g_chatPrints[0].text == viptest::Colorize(kReceivedDisabledPhrase),
		  "timeout 0 prints ReceivedDisabled");

	// ... an expired value is not a second chance either while timeout is 0.
	g_cookieValues[".vip_test"] = std::to_string(static_cast<long long>(std::time(nullptr)) - 10);
	g_chatPrints.clear();
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled, "an expired value answers Handled");
	CheckEqInt(g_giveCalls, 0, "timeout 0 never grants an expired value");
	Check(g_chatPrints.size() == 1 && g_chatPrints[0].text == viptest::Colorize(kReceivedDisabledPhrase),
		  "an expired value with timeout 0 also prints ReceivedDisabled");

	// ... while an empty cookie still grants (the original's !strlen short-circuit).
	g_cookieValues.clear();
	g_giveCalls = 0;
	g_setCookieCalls = 0;
	g_chatPrints.clear();
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled, "an empty cookie answers Handled");
	CheckEqInt(g_giveCalls, 1, "an empty cookie grants even with timeout 0");
	CheckEqInt(g_setCookieCalls, 1, "the zero timeout is part of the new stored value");
	g_config.timeout = 172800;

	// --- a VIP player -------------------------------------------------------------------------
	g_clientIsVip = true;
	g_giveCalls = 0;
	g_getCookieCalls = 0;
	g_chatPrints.clear();
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled, "a VIP answers Handled");
	CheckEqInt(g_giveCalls, 0, "a VIP is not given a second period");
	CheckEqInt(g_getCookieCalls, 0, "a VIP's cookie is not read");
	Check(g_chatPrints.size() == 1 && g_chatPrints[0].text == viptest::Colorize(kAlreadyVipPhrase),
		  "a VIP gets AlreadyVIP, colourised");
	g_clientIsVip = false;

	// --- a bot and the server console never reach s2sdk or the vip core ------------------------
	g_clients[1].fake = true;
	ResetClientCounters();
	g_vipIsClientVipCalls = 0;
	g_giveCalls = 0;
	g_chatPrints.clear();
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled, "a bot answers Handled");
	CheckEqInt(g_vipIsClientVipCalls, 0, "a bot never reaches VIP_IsClientVIP");
	CheckEqInt(g_giveCalls, 0, "a bot is never given VIP");
	Check(g_chatPrints.empty(), "a bot gets no chat line");
	CheckEqInt(g_fakeCalls, 1, "the bot is refused by IsFakeClient");
	ConnectClient(1);

	g_serverPrints.clear();
	ResetClientCounters();
	g_vipIsClientVipCalls = 0;
	Check(g_plugin.OnVipTestCommand(-1) == s2sdk::ResultType::Handled, "the server console answers Handled");
	CheckEqInt(g_vipIsClientVipCalls, 0, "the server console never reaches VIP_IsClientVIP");
	CheckEqInt(g_connectedCalls, 0, "the server console never reaches s2sdk");
	Check(g_serverPrints.size() == 1, "the server console gets one hint line");

	// --- OnMapEnd: the handler touches nothing until the next map ------------------------------
	FireMapEnd();
	ResetClientCounters();
	g_vipIsClientVipCalls = 0;
	g_giveCalls = 0;
	g_chatPrints.clear();
	Check(!viptest::IsMapReady(), "OnMapEnd closes the gate");
	Check(g_plugin.OnVipTestCommand(1) == s2sdk::ResultType::Handled, "the map-change window answers Handled");
	CheckEqInt(g_connectedCalls, 0, "no client query in the map-change window");
	CheckEqInt(g_vipIsClientVipCalls, 0, "no vip call in the map-change window");
	CheckEqInt(g_giveCalls, 0, "no VIP is given in the map-change window");
	Check(g_chatPrints.empty(), "no chat line in the map-change window");

	// --- OnPluginEnd: every registration and listener is detached (PITFALLS A7) ---------------
	g_removedCommands = 0;
	g_removedListeners = 0;
	g_readyUnregistrations = 0;
	g_mapStartUnregistrations = 0;
	g_mapEndUnregistrations = 0;
	Check(g_plugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "OnPluginEnd succeeds");
	CheckEqInt(g_readyUnregistrations, 1, "VIP_OnVIPLoaded is unregistered");
	CheckEqInt(g_removedCommands, 6, "the six owned commands are removed");
	CheckEqInt(g_removedListeners, 2, "the say/say_team listeners are removed");
	CheckEqInt(g_mapStartUnregistrations, 1, "the OnMapStart listener is removed");
	CheckEqInt(g_mapEndUnregistrations, 1, "the OnMapEnd listener is removed");
	Check(!viptest::IsMapReady(), "the gate is shut after OnPluginEnd");

	// --- reload: OnPluginStart arms everything again ------------------------------------------
	g_mapStartRegistrations = 0;
	g_mapEndRegistrations = 0;
	g_readyRegistrations = 0;
	Check(g_plugin.OnPluginStart().GetCode() == plg::PluginCode::Ok, "a reload start succeeds");
	CheckEqInt(g_mapStartRegistrations, 1, "a reload registers the OnMapStart listener once");
	CheckEqInt(g_mapEndRegistrations, 1, "a reload registers the OnMapEnd listener once");
	CheckEqInt(g_readyRegistrations, 1, "a reload registers VIP_OnVIPLoaded once");
	Check(g_plugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok, "the reloaded plugin unloads cleanly");
}

void ShutdownFakes() {
	// Both modules must already be detached; the calls are idempotent and must not touch a binding.
	viptest::RemoveCommands();
	viptest::ShutdownMapGate();
	Check(!viptest::IsMapReady(), "the gate is shut at the end of the run");
}

void PrintSummary() {
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
}

} // namespace

int main() {
	TestKv1();
	TestColorize();
	TestFormatPhrase();
	TestChatWords();
	TestGrantDecision();
	TestCookieToString();
	TestConfig();
	TestGuards();
	TestCommands();
	TestPluginStartWithoutVipBindings();
	TestPluginFlow();
	ShutdownFakes();
	PrintSummary();
	return g_failures == 0 ? 0 : 1;
}
