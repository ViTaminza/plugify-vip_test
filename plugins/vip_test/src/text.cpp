#include "text.hpp"

#include <array>
#include <cstdio>
#include <vector>

namespace viptest {
namespace {

// cs2-menus/utils.h colors_text[] / colors_hex[], same order and bytes as pisexcompat and s2guard.
// The last entry is the chat line break: "\n" (backslash-n in the phrase file) -> U+2029.
struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
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
												  {"\\n", "\xe2\x80\xa9"}}};

std::string ReplaceTags(std::string_view text, bool strip) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement =
				strip ? (color.tag == "\\n" ? std::string_view("\n") : std::string_view()) : color.bytes;
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

} // namespace

std::string Colorize(std::string_view text) {
	return ReplaceTags(text, /*strip=*/false);
}

std::string StripColorTags(std::string_view text) {
	return ReplaceTags(text, /*strip=*/true);
}

std::string FormatV(const char* format, va_list args) {
	if (format == nullptr) {
		return {};
	}
	va_list copy;
	va_copy(copy, args);
	const int size = std::vsnprintf(nullptr, 0, format, copy);
	va_end(copy);
	if (size <= 0) {
		return {};
	}
	std::vector<char> buffer(static_cast<std::size_t>(size) + 1);
	std::vsnprintf(buffer.data(), buffer.size(), format, args);
	return std::string(buffer.data(), static_cast<std::size_t>(size));
}

std::string Format(const char* format, ...) {
	va_list args;
	va_start(args, format);
	std::string out = FormatV(format, args);
	va_end(args);
	return out;
}

std::string FormatPhrase(const char* format, ...) {
	va_list args;
	va_start(args, format);
	std::string out = FormatV(format, args);
	va_end(args);
	return out;
}

} // namespace viptest
