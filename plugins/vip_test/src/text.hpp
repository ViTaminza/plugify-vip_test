// vip_test — chat formatting and the cs2-menus colour tags.
//
// The phrase files (vip.phrases.txt) carry the cs2-menus tags ({RED}, {PURPLE}, ...). s2sdk's
// PrintToChat sends the text as it is, and its PrintToChatColored uses a different tag table, so
// the plugin converts them itself with the byte-identical table the Metamod servers used
// (cs2-menus utils.h colors_text[]/colors_hex[], the same table pisexcompat and s2guard had).

#pragma once

#include <cstdarg>
#include <string>
#include <string_view>

namespace viptest {

// Replaces every cs2-menus tag with the chat control byte; the "\n" tag becomes U+2029 (the
// in-chat line break). Unknown text is left alone.
std::string Colorize(std::string_view text);

// The same tags removed; the "\n" tag becomes a real newline (server-console output).
std::string StripColorTags(std::string_view text);

std::string FormatV(const char* format, va_list args);

#if defined(__GNUC__) || defined(__clang__)
std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));
#else
std::string Format(const char* format, ...);
#endif

// The original passed every phrase (AlreadyReceived = "... %s!", but also AlreadyVIP and
// ReceivedDisabled with no arguments) straight to pisex::PrintToChat, which used it as the printf
// FORMAT. The port keeps that semantics: a phrase containing "%%" prints "%". This helper exists
// because a phrase with no arguments is not a string literal, which GCC reports as
// -Wformat-security when it goes through Format() directly; handing the call's own va_list to
// FormatV keeps the behaviour without the warning. Deliberately no printf attribute: the phrases
// are data, not format literals to check.
std::string FormatPhrase(const char* format, ...);

} // namespace viptest
