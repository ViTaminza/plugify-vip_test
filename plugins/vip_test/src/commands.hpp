// vip_test — console and chat command registration on top of s2sdk (PITFALLS.md C11).
//
// What s2sdk does (con_command_manager.cpp, 3.0.x):
//   * a "!name" / "/name" line in chat is cut after the trigger, tokenised and dispatched to the
//     AddConsoleCommand callback with ConCommandContext::Chat; no ConVar flag is checked on that
//     path, so the callback must decide which sources a name answers for;
//   * a line typed without a trigger ("viptest") is not a command at all: it only reaches the
//     "say"/"say_team" command listeners.
//
// This module keeps the split the original Metamod plugin had: "mm_viptest"/"sm_viptest" and
// "mm_testvip"/"sm_testvip" answer in a console only (a client console included), "viptest" and
// "testvip" answer the "!"/"+" chat trigger, and the bare words are matched on say/say_team.
// Every registration is removed in OnPluginEnd (PITFALLS A7), and a command name that already
// existed (which s2sdk cannot destroy) loses only our listener.

#pragma once

#include <cstdint>
#include <string_view>

#include <s2sdk/s2sdk/enums.hpp>

namespace viptest {

// Which input paths a registered name answers on.
enum class CommandFrom : uint8_t {
	Console = 1, // typed in a console: the server console / rcon (caller -1) or a client console
	Chat = 2,    // "!name" or "/name" in chat
	Both = 3,
};

// The plugin only needs the caller slot; the arguments are deliberately not forwarded.
using CommandHandler = s2sdk::ResultType (*)(int32_t iSlot);

// Registers `name` for the given sources and makes it client-executable (CS2 needs
// ConVarFlag::ClientCanExecute|Release for a client console command; the chat path ignores the
// flags but is filtered by `from` in the trampoline). Returns false on a bad name/handler or a
// missing s2sdk binding.
bool AddCommand(std::string_view name, CommandFrom from, CommandHandler handler, std::string_view description);

// Registers a word typed in chat without a "!" or "/" trigger (the say/say_team listeners).
bool AddChatWord(std::string_view word, CommandHandler handler);

// Removes every command, say/say_team listener and word registered here, so `plg reload` leaves
// nothing pointing into the unloaded plugin. Safe to call twice.
void RemoveCommands();

} // namespace viptest
