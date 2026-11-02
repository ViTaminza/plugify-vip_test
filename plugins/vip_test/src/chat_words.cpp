#include "chat_words.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace viptest {
namespace {

char Lower(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::string ToLower(std::string_view text) {
	std::string out(text);
	std::transform(out.begin(), out.end(), out.begin(), Lower);
	return out;
}

} // namespace

std::vector<std::string> SplitChatWords(std::string_view line) {
	std::vector<std::string> words;
	std::size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
			++i;
		}
		if (i >= line.size()) {
			break;
		}
		std::string word;
		if (line[i] == '"') {
			++i;
			while (i < line.size() && line[i] != '"') {
				word.push_back(line[i++]);
			}
			if (i < line.size()) {
				++i; // the closing quote
			}
		} else {
			while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
				word.push_back(line[i++]);
			}
		}
		words.push_back(std::move(word));
	}
	return words;
}

std::string FirstChatWord(std::string_view line) {
	std::vector<std::string> words = SplitChatWords(line);
	if (words.empty()) {
		return {};
	}
	return ToLower(words.front());
}

bool IsBareChatWord(std::string_view word) {
	return !word.empty() && word.front() != '!' && word.front() != '/';
}

bool MatchesChatWord(std::string_view line, std::string_view word) {
	if (!IsBareChatWord(word)) {
		return false;
	}
	const std::string first = FirstChatWord(line);
	if (first.empty() || !IsBareChatWord(first)) {
		return false;
	}
	return first == ToLower(word);
}

bool MatchesChatWordToken(std::string_view token, std::string_view word) {
	if (!IsBareChatWord(token) || !IsBareChatWord(word)) {
		return false;
	}
	return ToLower(token) == ToLower(word);
}

} // namespace viptest
