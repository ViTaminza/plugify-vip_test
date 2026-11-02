#include "guards.hpp"

#include "common.hpp"
#include "log.hpp"
#include "text.hpp"

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <string>

namespace viptest {
namespace {

// --- map gate state ---------------------------------------------------------------------------
//
// The same three flags (and the same state machine) pisexcompat and s2guard used, driven by the
// s2sdk listeners directly:
//   g_mapReady       true while a map is up: every client call is allowed
//   g_mapStartSeen   an OnMapStart arrived since start / the last OnMapEnd
//   g_mapEndSeen     an OnMapEnd arrived since start
bool g_mapReady = false;
bool g_mapStartSeen = false;
bool g_mapEndSeen = false;
bool g_listenersRegistered = false;

// s2sdk listeners: plain function pointers, called on the game thread. Nothing here may throw,
// and no client/entity call is made: only state changes.
void OnMapStartListener() {
	// OnMapStart is the map's first live frame; the gate is open for everything that follows.
	// (A plugin loaded into a running map never sees this callback - PollMapGate() covers it.)
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
}

void OnMapEndListener() {
	// From here until the next OnMapStart every s2sdk client query can crash (PITFALLS B2): the
	// client list is being torn down. State only, no client call.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

} // namespace

void InitMapGate() {
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	if (g_listenersRegistered) {
		return;
	}
	if (__s2sdk_OnMapStart_Register && __s2sdk_OnMapEnd_Register) {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
		g_listenersRegistered = true;
	} else {
		// Without the map-end listener nothing could take readiness away again, so the gate stays
		// closed for good (PITFALLS A1: log and stay inactive, never return Failed).
		LogError(Format("[Error] [%s] the s2sdk map listeners are unavailable - client calls stay disabled",
						kLogTag));
	}
}

void PollMapGate() {
	if (!g_listenersRegistered) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first OnPluginUpdate after
	// OnPluginStart is the proof of a live map (the host pumps updates from the server frame).
	// After an OnMapEnd only a new OnMapStart re-opens the gate: the host keeps pumping updates
	// through a changelevel, so `!g_mapEndSeen` must not be enough once one was seen.
	if (!g_mapReady && (g_mapStartSeen || !g_mapEndSeen)) {
		g_mapReady = true;
	}
}

void ShutdownMapGate() {
	if (g_listenersRegistered) {
		g_listenersRegistered = false;
		if (__s2sdk_OnMapStart_Unregister) {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		}
		if (__s2sdk_OnMapEnd_Unregister) {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		}
	}
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
}

bool IsMapReady() {
	return g_mapReady;
}

// --- client checks ----------------------------------------------------------------------------

bool IsSlotInRange(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

bool HasClient(int32_t slot) {
	if (!g_mapReady || !IsSlotInRange(slot)) {
		return false;
	}
	// PITFALLS C2: connected -> in game -> not SourceTV. The layers after the first refusal are
	// never reached, so a stale or empty slot costs exactly one export call.
	if (!__s2sdk_IsClientConnected || !s2sdk::IsClientConnected(slot)) {
		return false;
	}
	if (!__s2sdk_IsClientInGame || !s2sdk::IsClientInGame(slot)) {
		return false;
	}
	if (!__s2sdk_IsClientSourceTV || s2sdk::IsClientSourceTV(slot)) {
		return false;
	}
	// PITFALLS B5: bots have no SteamID and no net channel; IsFakeClient is safe only while a map
	// is up, which the gate above guarantees.
	if (!__s2sdk_IsFakeClient || s2sdk::IsFakeClient(slot)) {
		return false;
	}
	return true;
}

uint64_t ClientSteamID64(int32_t slot) {
	if (!HasClient(slot) || !__s2sdk_GetClientSteamID64) {
		return 0;
	}
	return s2sdk::GetClientSteamID64(slot);
}

// --- output -----------------------------------------------------------------------------------

void PrintToChat(int32_t slot, const std::string& text) {
	if (slot < 0) {
		// The server console: no client query at all, only the tags removed. PrintToServer is
		// ConMsg("%s\n"), so one trailing newline of the text is dropped to stay on one line.
		if (!__s2sdk_PrintToServer) {
			return;
		}
		std::string line = StripColorTags(text);
		if (!line.empty() && line.back() == '\n') {
			line.pop_back();
		}
		s2sdk::PrintToServer(plg::string(line.data(), line.size()));
		return;
	}
	if (!HasClient(slot) || !__s2sdk_PrintToChat) {
		return;
	}
	const std::string colored = Colorize(text);
	s2sdk::PrintToChat(slot, plg::string(colored.data(), colored.size()));
}

} // namespace viptest
