// vip_test — Plugify port of the Pisex VIP module "VIP_Test" / "[VIP] VIP Test"
// (ref/src/cs2-vip-modules/VIP_Test/source/vip_test.cpp, 142 lines).
//
// Behaviour kept identical: a chat/console command hands a free VIP period to a player who does
// not have VIP yet, at most once per "timeout" seconds (0 = only once ever):
//
//   commands   mm_viptest / sm_viptest / !viptest / viptest
//              mm_testvip / sm_testvip / !testvip / testvip
//   config     addons/configs/vip/vip_test.ini -> "VIP" { "group" "VIPTEST" "time" "1800"
//              "timeout" "172800" } (the values of the production server's file)
//
//   * a player who is already a VIP gets the "AlreadyVIP" phrase;
//   * otherwise the cooldown is read from the "vip_test" cookie (keyed as '<group>.vip_test',
//     which for a player without a group is ".vip_test" - the original's VIP_GetClientCookie
//     composes the name the same way); an empty or (unless timeout is 0) expired value stores
//     now + time + timeout and calls VIP_GiveClientVIP(slot, time, group);
//   * a running cooldown prints "AlreadyReceived" with the stored expiry as %Y-%m-%d %H:%M:%S
//     (local time), or "ReceivedDisabled" when timeout is 0.
//
// Port notes:
//   * Pure s2sdk: there is no s2guard and no pisexcompat. Every safety rule the plugin needs lives
//     in src/ (PLUGIN_STANDARD section 7):
//       guards.cpp     map gate (OnMapStart/OnMapEnd listeners, first update for a late load) and
//                      HasClient()/ClientSteamID64() (PITFALLS B1/B2, C2, B5) plus guarded output;
//       commands.cpp   console/chat command registration and the say/say_team word listener
//                      (PITFALLS C11), detached in OnPluginEnd (A7);
//       text.cpp       Format/FormatV/FormatPhrase and the cs2-menus colour tags;
//       kv1.cpp        the KeyValues reader for vip_test.ini;
//       chat_words.cpp the bare chat word matching;
//       config.cpp     <GetConfigsDir()>/vip/vip_test.ini (fallback: vip_test/vip_test.ini);
//       grant.cpp      the three lines of the original's handler as a pure decision (the cooldown
//                      and the "AlreadyReceived"/"ReceivedDisabled" choice), so the tests cover
//                      them without an engine.
//   * The core (IVIPApi) is the Plugify "vip" plugin; the module calls it through the consumer
//     wrapper of plugins/vip/include/vip_api.hpp. The cookie stays in the official permissions
//     plugin under the same composed name, exactly like the original's ICookiesApi cookie - but
//     the module reads and writes it itself: the core's VIP_Get/SetClientCookie answer ""
//     /refuse the write while the player has no VIP record (an empty group), which is precisely
//     the state this module probes (see CookieKey()).
//   * The original's client calls are all behind viptest::HasClient(): a map must be up, the slot
//     must be 0..63, connected, in game, not SourceTV and not a bot. The server console caller
//     (-1) and a client that left therefore never reach s2sdk or the vip core.
//   * s2sdk does not validate arguments (PITFALLS C2) and reading a client in the map-change
//     window crashes (B2), which is why the handler refuses a slot that fails HasClient() instead
//     of calling VIP_IsClientVIP() with it.
//   * The original loaded the config in Load() and refused to start without it; the port fails
//     soft (logs the missing file, keeps the production values as defaults), because a plugin
//     that returns Failed stops every plugify plugin (PITFALLS A1).
//   * std::stoi of the original threw on a malformed cookie; the port parses non-throwing and
//     logs, so a corrupted cookie cannot take the server down from a command handler.
//   * The commands are registered in VIP_OnVIPLoaded (once the core's database is up), like the
//     original, with the API's late-load poll (VIP_IsVIPLoaded) for a module that is loaded
//     after the core already finished. The handler always answers Handled: unlike Metamod, s2sdk
//     only swallows the public "!" say line on that result, not on the original's "no supercede".

#include <permissions/permissions.hpp>
#include <plg/any.hpp>
#include <plg/plugin.hpp>
#include <s2sdk/s2sdk/enums.hpp>

#include <vip_api.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <optional>
#include <string>

#include "commands.hpp"
#include "common.hpp"
#include "config.hpp"
#include "grant.hpp"
#include "guards.hpp"
#include "log.hpp"
#include "text.hpp"

namespace {

// The object the ported module uses (the Pisex g_pVIPCore).
vip::IVIPApi* g_pVIPCore = &vip::IVIPApi::Get();

// The original's szGroup / iTime / iTimeout, read from vip_test.ini in OnPluginStart.
viptest::VipTestConfig g_config;

class VipTestPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// VIP_OnVIPLoaded of the original: the core's database is up, the commands may be used.
	void OnCoreReady();

	// The handler of every spelling; always Handled (the old callback answered true).
	s2sdk::ResultType OnVipTestCommand(int32_t iSlot);

private:
	// VIP_GetClientCookie(iSlot, "vip_test") / VIP_SetClientCookie(iSlot, "vip_test", data),
	// composed as '<group>.vip_test' - implemented on the permissions plugin, see CookieKey().
	std::string CookieKey(int32_t iSlot);
	std::string ReadTestCookie(int32_t iSlot);
	bool WriteTestCookie(int32_t iSlot, const std::string& data);

	// VIP_OnVIPLoaded may fire once; the late-load path must not register a second time.
	bool m_coreReady = false;
};

VipTestPlugin g_plugin;

// The command/chat-word handler registered in OnCoreReady (the commands module takes a plain
// function pointer, so it cannot point into a member function).
s2sdk::ResultType VipTestCommandHandler(int32_t iSlot) {
	return g_plugin.OnVipTestCommand(iSlot);
}

// --- the cooldown cookie (the original's VIP_Get/SetClientCookie) ---------------------------------
//
// The core reads and writes '<group>.<name>' cookies only while the player has a VIP record with a
// known group (VIP_GetClientCookie/VIP_SetClientCookie return early on an empty group), but this
// module probes its cooldown cookie exactly when the player has no record: the original composed
// the name with the empty group and the Pisex cookie store answered ".vip_test". The helpers below
// therefore use the official permissions plugin directly - a read is addressed by name and ignores
// the writer, and a write needs an id only for the storage routing of perm_storage - with the very
// same composed key, so the semantics of the original hold for non-VIPs. (Reported as a divergence
// of the core's cookie methods.)

std::string VipTestPlugin::CookieKey(int32_t iSlot) {
	// VIP_GetClientCookie composes "%s.%s" with the group of the player; a player without a VIP
	// record has no group, so the key is ".vip_test" - exactly the original's.
	return std::string(g_pVIPCore->VIP_GetClientVIPGroup(iSlot)) + "." + viptest::kCookieName;
}

std::string VipTestPlugin::ReadTestCookie(int32_t iSlot) {
	if (__permissions_GetCookie == nullptr) {
		return {}; // no cookie store: VIP_GetClientCookie of the core answers "" as well
	}
	const uint64_t steamid = viptest::ClientSteamID64(iSlot);
	if (steamid == 0) {
		return {};
	}
	try {
		plg::any value;
		if (permissions::GetCookie(steamid, plg::string(CookieKey(iSlot)), value) != permissions::Status::Success) {
			return {};
		}
		return viptest::CookieToString(value);
	} catch (const std::exception& ex) {
		viptest::LogError(viptest::Format("[Error] [%s] reading the vip_test cookie failed: %s", viptest::kLogTag,
										  ex.what()));
	} catch (...) {
		viptest::LogError(viptest::Format("[Error] [%s] reading the vip_test cookie failed with an unknown exception",
										  viptest::kLogTag));
	}
	return {};
}

bool VipTestPlugin::WriteTestCookie(int32_t iSlot, const std::string& data) {
	if (__permissions_SetCookie == nullptr) {
		return false;
	}
	const uint64_t steamid = viptest::ClientSteamID64(iSlot);
	if (steamid == 0) {
		return false;
	}
	try {
		// The id attributes the write to this plugin for the storage routing; the cookie itself is
		// stored per user under its name.
		const permissions::Status status =
				permissions::SetCookie(static_cast<int64_t>(GetId()), steamid,
									   plg::string(CookieKey(iSlot)), plg::any(plg::string(data)),
									   /*dontBroadcast=*/false);
		return status == permissions::Status::Success;
	} catch (const std::exception& ex) {
		viptest::LogError(viptest::Format("[Error] [%s] writing the vip_test cookie failed: %s", viptest::kLogTag,
										  ex.what()));
	} catch (...) {
		viptest::LogError(viptest::Format("[Error] [%s] writing the vip_test cookie failed with an unknown exception",
										  viptest::kLogTag));
	}
	return false;
}

s2sdk::ResultType VipTestPlugin::OnVipTestCommand(int32_t iSlot) {
	if (!viptest::HasClient(iSlot)) {
		// -1 is the server console (the command list also registers the console names, so the
		// server itself can type it) and every other refused slot is a client that left or a slot
		// s2sdk cannot query. No vip/s2sdk client call may be made for either (PITFALLS B2/C2).
		if (iSlot < 0) {
			viptest::PrintToChat(iSlot, "[VIP-TEST] This command must be typed by a player in game.");
		}
		return s2sdk::ResultType::Handled;
	}

	// One timestamp for the decision and for the value the grant stores (the original called
	// std::time(0) in both places).
	const std::time_t now = std::time(nullptr);
	const bool isVip = g_pVIPCore->VIP_IsClientVIP(iSlot);
	// const char* szValue = VIP_GetClientCookie(iSlot, "vip_test")
	const std::string cookie = isVip ? std::string() : ReadTestCookie(iSlot);

	// !strlen(szValue) || ((std::stoi(szValue) < std::time(0)) && iTimeout != 0)
	const viptest::Decision decision = viptest::Decide(isVip, cookie, g_config.timeout, now);
	if (decision.invalidCookie) {
		// std::stoi would have thrown here; the port logs and treats the value as expired.
		viptest::LogError(viptest::Format("[Error] [%s] the vip_test cookie of slot %i is not a timestamp ('%s')",
										  viptest::kLogTag, iSlot, cookie.c_str()));
	}

	switch (decision.outcome) {
	case viptest::Outcome::Give:
		// VIP_SetClientCookie(iSlot, "vip_test", std::to_string(std::time(0) + iTime + iTimeout))
		WriteTestCookie(iSlot, std::to_string(viptest::NextCookieValue(g_config.time, g_config.timeout, now)));
		// VIP_GiveClientVIP(iSlot, iTime, szGroup)
		g_pVIPCore->VIP_GiveClientVIP(iSlot, g_config.time, g_config.group.c_str());
		break;

	case viptest::Outcome::AlreadyReceived:
	case viptest::Outcome::ReceivedDisabled: {
		// time_t currentTime_t = (time_t)std::stoi(szValue); std::strftime(..., "%Y-%m-%d %H:%M:%S",
		// std::localtime(&currentTime_t))
		const std::optional<std::string> formatted = viptest::FormatLocalTime(decision.stored);
		std::string buffer;
		if (formatted) {
			buffer = *formatted;
		} else {
			// A cookie this module did not write (a hand edit or another plugin) can hold a value
			// std::localtime cannot represent; glibc answers NULL. Print the raw value instead of
			// passing NULL to std::strftime.
			viptest::LogError(viptest::Format("[Error] [%s] the vip_test cookie of slot %i is out of range ('%s')",
											  viptest::kLogTag, iSlot, cookie.c_str()));
			buffer = std::to_string(decision.stored);
		}
		// g_pUtils->PrintToChat(iSlot, VIP_GetTranslate(iTimeout == 0 ? "ReceivedDisabled"
		//                                                             : "AlreadyReceived"), buffer)
		const char* phrase = g_pVIPCore->VIP_GetTranslate(
				decision.outcome == viptest::Outcome::ReceivedDisabled ? "ReceivedDisabled" : "AlreadyReceived");
		viptest::PrintToChat(iSlot, viptest::FormatPhrase(phrase, buffer.c_str()));
		break;
	}

	case viptest::Outcome::AlreadyVip:
		// g_pUtils->PrintToChat(iSlot, VIP_GetTranslate("AlreadyVIP"))
		viptest::PrintToChat(iSlot, viptest::FormatPhrase(g_pVIPCore->VIP_GetTranslate("AlreadyVIP")));
		break;
	}

	// true = handled: s2sdk only stops the public "!" say line from being broadcast on Handled
	// (the old callback's true; the original's "no supercede" echoed every "!viptest").
	return s2sdk::ResultType::Handled;
}

// --- lifecycle ---------------------------------------------------------------------------------

plg::PluginResult VipTestPlugin::OnPluginStart() {
	try {
		// Map listeners first: they gate every client call this plugin ever makes.
		viptest::InitMapGate();
		g_config = viptest::LoadVipTestConfig();

		g_pVIPCore->VIP_OnVIPLoaded([] { g_plugin.OnCoreReady(); });
		if (g_pVIPCore->VIP_IsVIPLoaded()) {
			OnCoreReady(); // vip was already up (plugin reloaded mid-map)
		}

		return {};
	} catch (const std::exception& ex) {
		viptest::LogError(viptest::Format("[Error] [%s] OnPluginStart failed: %s", viptest::kLogTag, ex.what()));
	} catch (...) {
		viptest::LogError(viptest::Format("[Error] [%s] OnPluginStart failed with an unknown exception",
										  viptest::kLogTag));
	}
	return {}; // never Failed: that stops every plugify plugin (PITFALLS A1)
}

plg::PluginResult VipTestPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		viptest::PollMapGate();
	} catch (const std::exception& ex) {
		viptest::LogError(viptest::Format("[Error] [%s] OnPluginUpdate failed: %s", viptest::kLogTag, ex.what()));
	} catch (...) {
		viptest::LogError(viptest::Format("[Error] [%s] OnPluginUpdate failed with an unknown exception",
										  viptest::kLogTag));
	}
	return {};
}

plg::PluginResult VipTestPlugin::OnPluginEnd() {
	try {
		g_pVIPCore->VIP_OnVIPLoadedUnregister();
	} catch (...) {
		// the vip core is going away too; nothing left to do for its callback
	}
	try {
		viptest::RemoveCommands(); // every command and say/say_team listener (PITFALLS A7)
	} catch (...) {
	}
	try {
		viptest::ShutdownMapGate(); // the map listeners point into this library
	} catch (...) {
	}
	return {};
}

void VipTestPlugin::OnCoreReady() {
	if (m_coreReady) {
		return;
	}
	m_coreReady = true;

	// RegCommand(g_PLID, {"mm_viptest", "sm_viptest"}, {"!viptest", "viptest"}, OnVIPTestCommand) and
	// RegCommand(g_PLID, {"mm_testvip", "sm_testvip"}, {"!testvip", "testvip"}, OnVIPTestCommand).
	// "mm_*"/"sm_*" answer in a console only (a client console included), "!viptest"/"!testvip"
	// answer in chat through the same callback, and the bare words come from the say/say_team
	// listeners (PITFALLS C11).
	viptest::AddCommand("mm_viptest", viptest::CommandFrom::Console, &VipTestCommandHandler,
						"Gives a free VIP period");
	viptest::AddCommand("sm_viptest", viptest::CommandFrom::Console, &VipTestCommandHandler,
						"Gives a free VIP period");
	viptest::AddCommand("mm_testvip", viptest::CommandFrom::Console, &VipTestCommandHandler,
						"Gives a free VIP period");
	viptest::AddCommand("sm_testvip", viptest::CommandFrom::Console, &VipTestCommandHandler,
						"Gives a free VIP period");
	viptest::AddCommand("viptest", viptest::CommandFrom::Chat, &VipTestCommandHandler,
						"Gives a free VIP period");
	viptest::AddCommand("testvip", viptest::CommandFrom::Chat, &VipTestCommandHandler,
						"Gives a free VIP period");
	viptest::AddChatWord("viptest", &VipTestCommandHandler);
	viptest::AddChatWord("testvip", &VipTestCommandHandler);
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
