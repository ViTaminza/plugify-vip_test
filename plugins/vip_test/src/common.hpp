// vip_test — constants shared by the plugin and its helpers.
//
// The plugin is pure s2sdk: every safety rule of PITFALLS.md that applies to a client query lives
// in guards.cpp / commands.cpp (see the header comment of plugin.cpp). There is no pisexcompat and
// no s2guard here (PLUGIN_STANDARD section 7).

#pragma once

#include <cstdint>

namespace viptest {

// The GetLogTag() of the original Pisex module ("[VIP-TEST]").
inline constexpr char kLogTag[] = "VIP-TEST";

// The config of the module inside the Plugify configs tree. The original file lives in the
// shared vip/ directory of the Metamod install (addons/configs/vip/vip_test.ini), so that path
// is tried first; a standard PLUGIN_STANDARD layout (<GetConfigsDir()>/<plugin name>/) is the
// fallback.
inline constexpr char kConfigFile[] = "vip/vip_test.ini";
inline constexpr char kConfigFileFallback[] = "vip_test/vip_test.ini";

// The values of the production vip_test.ini - the defaults when neither file can be read.
inline constexpr char kDefaultGroup[] = "VIPTEST";
inline constexpr int kDefaultTime = 1800;
inline constexpr int kDefaultTimeout = 172800;

// The cookie name of the original (VIP_GetClientCookie(iSlot, "vip_test")); the stored key is
// composed as '<group>.<name>'.
inline constexpr char kCookieName[] = "vip_test";

// CS2 has 64 player slots.
inline constexpr int32_t kMaxSlots = 64;

} // namespace viptest
