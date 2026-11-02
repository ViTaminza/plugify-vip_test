// vip_test — the plugin's own s2sdk safety guards (PITFALLS.md B1/B2, C2).
//
// The plugin calls s2sdk directly, so every rule s2guard used to enforce lives here:
//
//   * Map gate: no client or entity call is made before the first map, and none in the window
//     from OnMapEnd until the next OnMapStart (the client list is torn down there and s2sdk's
//     client lookups read an emptied list - PITFALLS B1/B2). The listeners are registered in
//     OnPluginStart, the gate is opened by OnMapStart (and by the first OnPluginUpdate, which
//     proves a live map for a plugin loaded mid-map), and closed by OnMapEnd.
//   * HasClient(): the full slot/client check the original Pisex utils did for every call, with
//     the extra !IsFakeClient layer PITFALLS B5 asks for (a bot has no SteamID and no net
//     channel, so it must never reach GetClientSteamID64 or the cookie store).
//   * Output: PrintToChat() only prints through the same check; a negative slot is the server
//     console, where the colour tags are stripped instead.

#pragma once

#include <cstdint>
#include <string>

namespace viptest {

// --- map gate ---------------------------------------------------------------------------------

// Registers the OnMapStart/OnMapEnd listeners. Touches no client; safe in OnPluginStart.
void InitMapGate();
// Per-frame pump (OnPluginUpdate): opens the gate for a plugin that loaded into a running map.
void PollMapGate();
// Removes the map listeners and closes the gate; called from OnPluginEnd so `plg reload` leaves
// nothing pointing into the unloaded library (PITFALLS A7).
void ShutdownMapGate();

// True while a map is up and client/entity calls are safe.
bool IsMapReady();

// --- client checks ----------------------------------------------------------------------------

bool IsSlotInRange(int32_t slot);

// The PITFALLS C2 gate, in order:
//   a map is up (B1/B2), 0 <= slot < 64, IsClientConnected, IsClientInGame, !IsClientSourceTV,
//   !IsFakeClient. Every s2sdk export is additionally checked against a missing binding, so a
//   host that did not fill one refuses the client instead of calling a null pointer.
bool HasClient(int32_t slot);

// The SteamID64 of a client that passed HasClient(), 0 otherwise (PITFALLS B5: never a bot, never
// an empty slot, never outside a live map).
uint64_t ClientSteamID64(int32_t slot);

// --- output -----------------------------------------------------------------------------------

// Chat output behind HasClient() and through the colour table of the phrase files. A negative slot
// is the server console (always available): the text goes to PrintToServer with the tags stripped.
void PrintToChat(int32_t slot, const std::string& text);

} // namespace viptest
