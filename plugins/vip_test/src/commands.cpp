#include "commands.hpp"

#include "chat_words.hpp"
#include "common.hpp"
#include "log.hpp"
#include "text.hpp"

#include <plg/string.hpp>
#include <plg/vector.hpp>
#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace viptest {
namespace {

constexpr uint8_t kConsoleSource = static_cast<uint8_t>(CommandFrom::Console);
constexpr uint8_t kChatSource = static_cast<uint8_t>(CommandFrom::Chat);
constexpr const char* kDefaultDescription = "Gives a free VIP period";

struct CommandEntry {
	std::string name; // lowercase
	uint8_t from = 0; // CommandFrom bits
	CommandHandler handler = nullptr;
};

struct ChatWordEntry {
	std::string word; // lowercase
	CommandHandler handler = nullptr;
};

struct S2Name {
	std::string name;
	bool owned = false; // AddConsoleCommand created it; false = we only listen on an existing one
};

std::vector<CommandEntry> g_commands;
std::vector<ChatWordEntry> g_chatWords;
std::vector<S2Name> g_s2Names;
bool g_sayHooked = false;

constexpr const char* kSayCommands[] = {"say", "say_team"};

std::string ToLower(std::string_view text) {
	std::string out(text);
	std::transform(out.begin(), out.end(), out.begin(),
				   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
	return out;
}

bool IsValidName(std::string_view name) {
	if (name.empty() || name.front() == '!' || name.front() == '/') {
		return false;
	}
	return std::none_of(name.begin(), name.end(),
						[](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0 || c == '"'; });
}

uint8_t Bits(CommandFrom from) {
	return static_cast<uint8_t>(from);
}

plg::string ToPlg(const std::string& text) {
	return plg::string(text.data(), text.size());
}

// Runs the handler of a matched command; a handler may not throw into s2sdk, so the trampolines
// below catch and log instead. The result is whatever the handler answered (Handled for vip_test:
// s2sdk then keeps the "!viptest" chat line from being broadcast - the old callback's true).
s2sdk::ResultType Run(CommandHandler handler, int32_t caller) {
	if (handler == nullptr) {
		return s2sdk::ResultType::Continue;
	}
	return handler(caller);
}

s2sdk::ResultType CommandTrampoline(int32_t caller, s2sdk::ConCommandContext context,
									const plg::vector<plg::string>& args) {
	try {
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const std::string name = ToLower(std::string_view(args[0].data(), args[0].size()));
		const uint8_t source = context == s2sdk::ConCommandContext::Chat ? kChatSource : kConsoleSource;
		for (const CommandEntry& entry : g_commands) {
			if (entry.name == name && (entry.from & source) != 0) {
				return Run(entry.handler, caller);
			}
		}
		// Registered for the other source only ("!mm_viptest" must not answer): leave the line to
		// the engine, exactly like pisexcompat's source filter did.
		return s2sdk::ResultType::Continue;
	} catch (const std::exception& ex) {
		LogError(Format("[Error] [%s] command handler threw: %s", kLogTag, ex.what()));
	} catch (...) {
		LogError(Format("[Error] [%s] command handler threw an unknown exception", kLogTag));
	}
	return s2sdk::ResultType::Handled;
}

// say/say_team hands over (say, message): a CS2 client sends the whole line as ONE quoted
// argument (it must be split), while s2sdk's console path has already tokenised `say viptest top`
// into arguments (args[1] is the first message word as typed and must not be split again).
bool MatchesSayLine(const plg::vector<plg::string>& args, std::string_view word) {
	if (args.size() < 2) {
		return false;
	}
	const std::string_view firstWord(args[1].data(), args[1].size());
	if (args.size() == 2) {
		return MatchesChatWord(firstWord, word);
	}
	return MatchesChatWordToken(firstWord, word);
}

s2sdk::ResultType ChatWordTrampoline(int32_t caller, s2sdk::ConCommandContext /*context*/,
									 const plg::vector<plg::string>& args) {
	try {
		if (args.size() < 2 || g_chatWords.empty()) {
			return s2sdk::ResultType::Continue;
		}
		for (const ChatWordEntry& entry : g_chatWords) {
			if (MatchesSayLine(args, entry.word)) {
				return Run(entry.handler, caller);
			}
		}
		return s2sdk::ResultType::Continue;
	} catch (const std::exception& ex) {
		LogError(Format("[Error] [%s] chat word handler threw: %s", kLogTag, ex.what()));
	} catch (...) {
		LogError(Format("[Error] [%s] chat word handler threw an unknown exception", kLogTag));
	}
	return s2sdk::ResultType::Handled;
}

bool EnsureS2Name(const std::string& name, std::string_view description) {
	for (const S2Name& known : g_s2Names) {
		if (known.name == name) {
			return true;
		}
	}
	if (!__s2sdk_AddConsoleCommand) {
		LogError(Format("[Error] [%s] AddConsoleCommand is unavailable - '%s' is not registered", kLogTag,
						name.c_str()));
		return false;
	}
	// Our commands are client-executable: "sm_viptest"/"mm_viptest" are typed in a client console
	// too, and the chat path ignores the flags anyway (the trampoline filters by source).
	const int64_t flags = static_cast<int64_t>(s2sdk::ConVarFlag::Release) |
						  static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute);
	// false = the name already existed (engine or another plugin): s2sdk attached our callback to
	// that command anyway, so RemoveCommands() must remove only the listener then.
	const bool owned = s2sdk::AddConsoleCommand(
			ToPlg(name), ToPlg(description.empty() ? std::string(kDefaultDescription) : std::string(description)),
			static_cast<s2sdk::ConVarFlag>(flags), &CommandTrampoline, s2sdk::HookMode::Pre);
	g_s2Names.push_back({name, owned});
	return true;
}

} // namespace

bool AddCommand(std::string_view name, CommandFrom from, CommandHandler handler, std::string_view description) {
	try {
		if (!IsValidName(name) || handler == nullptr || Bits(from) == 0) {
			LogError(Format("[Error] [%s] AddCommand('%s') rejected: bad name or handler", kLogTag,
							std::string(name).c_str()));
			return false;
		}
		const std::string lowered = ToLower(name);
		for (const CommandEntry& entry : g_commands) {
			if (entry.name == lowered && (entry.from & Bits(from)) != 0) {
				LogError(Format("[Error] [%s] command '%s' is already registered for that source", kLogTag,
							lowered.c_str()));
				return false;
			}
		}
		if (!EnsureS2Name(lowered, description)) {
			return false;
		}
		g_commands.push_back({lowered, Bits(from), handler});
		return true;
	} catch (const std::exception& ex) {
		LogError(Format("[Error] [%s] AddCommand failed: %s", kLogTag, ex.what()));
	} catch (...) {
		LogError(Format("[Error] [%s] AddCommand failed with an unknown exception", kLogTag));
	}
	return false;
}

bool AddChatWord(std::string_view word, CommandHandler handler) {
	try {
		if (!IsValidName(word) || handler == nullptr) {
			LogError(Format("[Error] [%s] AddChatWord('%s') rejected: bad word or handler", kLogTag,
							std::string(word).c_str()));
			return false;
		}
		const std::string lowered = ToLower(word);
		for (const ChatWordEntry& entry : g_chatWords) {
			if (entry.word == lowered) {
				LogError(Format("[Error] [%s] chat word '%s' is already registered", kLogTag, lowered.c_str()));
				return false;
			}
		}
		if (!g_sayHooked) {
			if (!__s2sdk_AddCommandListener) {
				LogError(Format("[Error] [%s] AddCommandListener is unavailable - the bare chat word is disabled",
						kLogTag));
				return false;
			}
			for (const char* say : kSayCommands) {
				if (!s2sdk::AddCommandListener(plg::string(say), &ChatWordTrampoline, s2sdk::HookMode::Pre)) {
					LogError(Format("[Error] [%s] could not listen on '%s'", kLogTag, say));
				}
			}
			g_sayHooked = true;
		}
		g_chatWords.push_back({lowered, handler});
		return true;
	} catch (const std::exception& ex) {
		LogError(Format("[Error] [%s] AddChatWord failed: %s", kLogTag, ex.what()));
	} catch (...) {
		LogError(Format("[Error] [%s] AddChatWord failed with an unknown exception", kLogTag));
	}
	return false;
}

void RemoveCommands() {
	for (const S2Name& known : g_s2Names) {
		if (known.owned) {
			if (__s2sdk_RemoveCommand) {
				s2sdk::RemoveCommand(ToPlg(known.name), &CommandTrampoline);
			}
		} else if (__s2sdk_RemoveCommandListener) {
			s2sdk::RemoveCommandListener(ToPlg(known.name), &CommandTrampoline, s2sdk::HookMode::Pre);
		}
	}
	if (g_sayHooked && __s2sdk_RemoveCommandListener) {
		for (const char* say : kSayCommands) {
			s2sdk::RemoveCommandListener(plg::string(say), &ChatWordTrampoline, s2sdk::HookMode::Pre);
		}
	}
	g_sayHooked = false;
	g_s2Names.clear();
	g_commands.clear();
	g_chatWords.clear();
}

} // namespace viptest
