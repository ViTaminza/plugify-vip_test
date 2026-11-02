// vip_test — the bare chat words ("viptest", "testvip") matched on say / say_team.
//
// s2sdk hands a say/say_team line to its listeners as (say, message): a CS2 client sends the whole
// line as ONE quoted argument, while a console `say viptest top` sends one argument per word. The
// plugin's commands.cpp joins the arguments back into a line and asks these helpers for the first
// word. A line that starts with a chat trigger ("!" or "/") belongs to AddConsoleCommand's chat
// path (ConCommandContext::Chat) and must not be answered twice, hence IsBareChatWord().

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace viptest {

// Whitespace-separated words of a chat line; double quotes group a word and are removed (the
// tokenizer the Metamod-era "say" handling used, kept so a quoted first word still matches).
std::vector<std::string> SplitChatWords(std::string_view line);

// The first word of a chat line, lowercased; "" when the line has none.
std::string FirstChatWord(std::string_view line);

// A bare chat word never starts with a chat trigger: "!viptest" and "/viptest" are
// AddConsoleCommand chat commands, not words for the say/say_team listener.
bool IsBareChatWord(std::string_view word);

// True when the first word of `line` is `word` (case-insensitive) and carries no chat trigger.
bool MatchesChatWord(std::string_view line, std::string_view word);

// True when the already-tokenised `token` is `word` (case-insensitive) and carries no chat
// trigger. s2sdk's console path splits `say viptest top` into arguments itself, so there the
// first message word is args[1] as typed and must not be split a second time.
bool MatchesChatWordToken(std::string_view token, std::string_view word);

} // namespace viptest
