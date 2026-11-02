// admin_system — implementation of the plugin's own s2sdk guards (see admin_guards.hpp).

#include "admin_guards.hpp"

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

namespace admin_system {
namespace {

// The same three flags (and the same state machine) the sibling pure-s2sdk plugins keep:
//   g_mapReady      a map is live - the only state in which a client/entity call is legal
//   g_mapStartSeen  an OnMapStart arrived since OnPluginStart / the last OnMapEnd
//   g_mapEndSeen    an OnMapEnd arrived since OnPluginStart
//   g_latchChecked  the mid-map latch has been decided (once per plugin instance)
bool g_mapReady = false;
bool g_mapStartSeen = false;
bool g_mapEndSeen = false;
bool g_latchChecked = false;
// Each listener is tracked on its own: a Register that throws after the other one succeeded must
// still be unregistered in ShutdownMapGate (otherwise the callback points into the unloaded .so
// after `plg reload`, PITFALLS A7).
bool g_mapStartRegistered = false;
bool g_mapEndRegistered = false;

MapLifecycleCallback g_onMapStart = nullptr;
MapLifecycleCallback g_onMapEnd = nullptr;

// s2sdk listeners: plain function pointers called on the game thread. Nothing here may throw and
// no client or entity call is made - only state changes.
void OnMapStartListener() {
	// The engine calls this once the new map and its entity system exist: readiness is granted here
	// for everything that follows (PITFALLS B1). A plugin loaded into a running map never sees this
	// callback - PollMapGate() covers it.
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
	g_latchChecked = true;
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down and every
	// client query crashes - the gate is dropped first and the hook only clears state.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
	g_latchChecked = true;
	if (g_onMapEnd != nullptr) {
		g_onMapEnd();
	}
}

// s2sdk's GetCurrentMap() null-checks gpGlobals itself, so it is the one map call that is safe
// before the first map (it answers an empty string there). "<empty>" is what the engine reports
// while no map file is loaded. A missing binding is NOT evidence of a live map (PITFALLS B1): the
// mid-map latch then stays shut and only a real OnMapStart may open the gate.
bool MapNamePresent() {
	if (__s2sdk_GetCurrentMap == nullptr) {
		return false;
	}
	try {
		const plg::string map = s2sdk::GetCurrentMap();
		const std::string_view name(map.data(), map.size());
		return !name.empty() && name != "<empty>";
	} catch (...) {
		return false;
	}
}

// --- the cs2-menus colour table (identical bytes to pisexcompat's Colorize) -------------------

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

// Pisex formats into char[512]; the admin core appends player names and reasons, so a little
// headroom avoids silent truncation. Anything longer is cut here, never in the engine.
constexpr std::size_t kBufferSize = 2048;

bool IsValidSlot(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

} // namespace

// --- map gate ---------------------------------------------------------------------------------

void SetMapLifecycleCallbacks(MapLifecycleCallback onMapStart, MapLifecycleCallback onMapEnd) {
	g_onMapStart = onMapStart;
	g_onMapEnd = onMapEnd;
}

bool MapListenerBindingsUsable() {
	return __s2sdk_OnMapStart_Register != nullptr && __s2sdk_OnMapStart_Unregister != nullptr &&
		   __s2sdk_OnMapEnd_Register != nullptr && __s2sdk_OnMapEnd_Unregister != nullptr;
}

void InitMapGate() {
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	g_latchChecked = false;
	if (g_mapStartRegistered || g_mapEndRegistered) {
		return;
	}
	// A registration without its matching Unregister would leave a callback in the unloaded .so
	// after `plg reload` (PITFALLS A7), so both pairs are required before ANY registration. A
	// partially present API leaves the gate shut for the whole instance (fail safe, PITFALLS A1:
	// log and stay inactive, never return Failed).
	if (!MapListenerBindingsUsable()) {
		LogError("admin_system: the s2sdk map listeners are unavailable (Register/Unregister pair "
				 "missing) - client calls stay disabled");
		return;
	}
	if (!g_mapStartRegistered) {
		try {
			s2sdk::OnMapStart_Register(&OnMapStartListener);
			g_mapStartRegistered = true;
		} catch (...) {
			LogError("admin_system: registering the s2sdk OnMapStart listener failed - client calls stay "
					 "disabled");
		}
	}
	if (!g_mapEndRegistered) {
		try {
			s2sdk::OnMapEnd_Register(&OnMapEndListener);
			g_mapEndRegistered = true;
		} catch (...) {
			LogError("admin_system: registering the s2sdk OnMapEnd listener failed - client calls stay disabled");
		}
	}
}

void PollMapGate() {
	// The latch needs the complete listener pair: with only half of it registered the gate could
	// never be closed again by OnMapEnd, so it stays shut (PITFALLS A7/B1).
	if (!g_mapStartRegistered || !g_mapEndRegistered || g_mapReady) {
		return;
	}
	// A plugin loaded into a running map never saw an OnMapStart: the first OnPluginUpdate after
	// OnPluginStart may prove a live map - but only while no OnMapEnd was seen (after one, only a
	// real OnMapStart may re-open the gate: the host keeps pumping updates through a changelevel)
	// and only when s2sdk reports a map name (a reload in the gap between OnMapEnd and the next
	// OnMapStart must not re-open the gate on an empty map, and a missing GetCurrentMap binding is
	// no proof either).
	if (g_latchChecked) {
		return;
	}
	g_latchChecked = true;
	if (g_mapEndSeen || !MapNamePresent()) {
		return;
	}
	g_mapReady = true;
	g_mapStartSeen = true;
	if (g_onMapStart != nullptr) {
		g_onMapStart();
	}
}

void ShutdownMapGate() {
	if (g_mapStartRegistered) {
		g_mapStartRegistered = false;
		if (__s2sdk_OnMapStart_Unregister != nullptr) {
			try {
				s2sdk::OnMapStart_Unregister(&OnMapStartListener);
			} catch (...) {
			}
		}
	}
	if (g_mapEndRegistered) {
		g_mapEndRegistered = false;
		if (__s2sdk_OnMapEnd_Unregister != nullptr) {
			try {
				s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
			} catch (...) {
			}
		}
	}
	// Drop readiness last: nothing can reach a client through this plugin after OnPluginEnd.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = false;
	g_latchChecked = true;
}

bool IsMapReady() {
	return g_mapReady;
}

// --- entity handles (PITFALLS C1) -------------------------------------------------------------

bool IsSlotInRange(int32_t slot) {
	return slot >= 0 && slot < kMaxSlots;
}

bool IsUsableEntityHandle(int32_t handle) {
	if (!g_mapReady || handle == 0 || handle == -1 || __s2sdk_IsValidEntHandle == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsValidEntHandle(handle);
	} catch (...) {
		return false;
	}
}

// --- slots and clients (PITFALLS C2) ----------------------------------------------------------

bool HasClient(int32_t slot) {
	if (!g_mapReady || !IsSlotInRange(slot) || __s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot);
	} catch (...) {
		return false;
	}
}

bool HasController(int32_t slot) {
	if (!HasClient(slot)) {
		return false;
	}
	if (__s2sdk_IsClientInGame == nullptr || __s2sdk_IsClientSourceTV == nullptr ||
		__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_PlayerSlotToEntHandle == nullptr ||
		__s2sdk_IsValidEntHandle == nullptr || __s2sdk_IsValidEntPointer == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientInGame(slot) || s2sdk::IsClientSourceTV(slot)) {
			return false;
		}
		// The controller entity: the pointer must exist, its handle must be usable and the engine
		// must still recognise both (C2: never pass a stale handle to a client/scoped call).
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr) {
			return false;
		}
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		return IsUsableEntityHandle(handle) && s2sdk::IsValidEntPointer(controller);
	} catch (...) {
		return false;
	}
}

bool HasPawn(int32_t slot) {
	if (!HasController(slot) || __s2sdk_GetClientPawn == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetClientPawn(slot) != nullptr;
	} catch (...) {
		return false;
	}
}

bool IsFakeClientSlot(int32_t slot) {
	if (!g_mapReady || !IsSlotInRange(slot) || __s2sdk_IsFakeClient == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsFakeClient(slot);
	} catch (...) {
		return true; // treat as unavailable rather than as a real player
	}
}

bool IsAuthorizedSlot(int32_t slot) {
	if (!g_mapReady || !IsSlotInRange(slot) || __s2sdk_IsClientAuthorized == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientAuthorized(slot);
	} catch (...) {
		return false;
	}
}

uint64_t ClientSteamID64(int32_t slot) {
	// The s2sdk call itself is null-safe, but for a slot that is not connected it answers from
	// Player::m_unauthenticatedSteamID - which keeps the *previous* occupant's id after a
	// disconnect. Pisex/Metamod returned 0 for a slot without a client; so do we.
	if (!HasClient(slot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

std::string ClientName(int32_t slot) {
	// Player::GetName dereferences the client *and* the controller without a check (a crash seen
	// right after a map start while the bots were being created), so the controller layer is part
	// of this guard.
	if (!HasController(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		const plg::string name = s2sdk::GetClientName(slot);
		std::string out(name.data(), name.size());
		// s2sdk answers "<unknown>" when it cannot resolve the client; the Pisex API returned an
		// empty string there, and callers treat "" as "no name".
		if (out == "<unknown>") {
			out.clear();
		}
		return out;
	} catch (...) {
		return {};
	}
}

std::string ClientIp(int32_t slot) {
	// Player::GetIpAddress reads the client's net channel, which is null for bots (and only exists
	// once the client is connected): answer "" instead of crashing.
	if (!HasClient(slot) || IsFakeClientSlot(slot) || __s2sdk_GetClientIp == nullptr) {
		return {};
	}
	try {
		const plg::string ip = s2sdk::GetClientIp(slot);
		return std::string(ip.data(), ip.size());
	} catch (...) {
		return {};
	}
}

void SetClientNameSafe(int32_t slot, const std::string& name) {
	// Player::SetName crashes for a slot without a client or without a controller (both are
	// dereferenced unchecked) - never call SetClientName unless the slot is fully in place.
	if (!HasController(slot) || __s2sdk_SetClientName == nullptr) {
		return;
	}
	try {
		s2sdk::SetClientName(slot, plg::string(name));
	} catch (...) {
	}
}

bool KickClientSlot(int32_t slot, int32_t reason) {
	if (!g_mapReady || !IsSlotInRange(slot) || __s2sdk_KickClient == nullptr) {
		return false;
	}
	try {
		s2sdk::KickClient(slot, static_cast<s2sdk::NetworkDisconnectionReason>(reason), plg::string());
		return true;
	} catch (...) {
		return false;
	}
}

// --- owner identity (stale menu request guard) ------------------------------------------------

bool CaptureOwner(int32_t slot, OwnerIdentity& out) {
	out = OwnerIdentity{};
	if (!IsSlotInRange(slot) || !HasController(slot) || __s2sdk_GetClientSteamID64 == nullptr) {
		return false;
	}
	try {
		const uint64_t steamId = s2sdk::GetClientSteamID64(slot);
		if (steamId == 0) {
			return false; // s2sdk's "no player" answer: never apply a request to it
		}
		const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
		if (!IsUsableEntityHandle(handle)) {
			return false;
		}
		// SteamID64 pins the account, the controller handle carries the entity serial - the two
		// identifiers together survive a slot being re-used by another player between frames.
		out = OwnerIdentity{slot, steamId, handle};
		return true;
	} catch (...) {
		return false;
	}
}

bool OwnerMatches(const OwnerIdentity& owner) {
	if (!owner.IsSet()) {
		return false;
	}
	OwnerIdentity current;
	if (!CaptureOwner(owner.slot, current)) {
		return false;
	}
	return current.steamId == owner.steamId && current.controllerHandle == owner.controllerHandle;
}

// --- output (Pisex's printer split) -----------------------------------------------------------

std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

std::string StripColorTags(std::string_view text) {
	std::string out(text);
	for (const ColorTag& color : kColorTags) {
		const std::string_view replacement = color.tag == "\\n" ? std::string_view("\n") : std::string_view();
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), replacement);
			pos += replacement.size();
		}
	}
	return out;
}

std::string FormatText(const char* format, ...) {
	std::array<char, kBufferSize> buffer{};
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	va_end(args);
	if (written <= 0) {
		return {};
	}
	return std::string(buffer.data(), static_cast<std::size_t>(written) < buffer.size() - 1
										  ? static_cast<std::size_t>(written)
										  : buffer.size() - 1);
}

void PrintChat(int32_t slot, const char* format, ...) {
	std::array<char, kBufferSize> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	va_end(args);
	if (!g_mapReady || !IsValidSlot(slot) || __s2sdk_PrintToChat == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChat(slot, plg::string(Colorize(buffer.data())));
	} catch (...) {
	}
}

void PrintChatAll(const char* format, ...) {
	std::array<char, kBufferSize> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	va_end(args);
	if (!g_mapReady || __s2sdk_PrintToChatAll == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToChatAll(plg::string(Colorize(buffer.data())));
	} catch (...) {
	}
}

void PrintConsole(int32_t slot, const char* format, ...) {
	std::array<char, kBufferSize> buffer{};
	va_list args;
	va_start(args, format);
	std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	va_end(args);

	if (slot == kServerConsoleSlot) {
		// Pisex: slot -1 is the server console (META_CONPRINT) - and exactly -1, no other negative
		// value (PITFALLS C11). Chat colour tags in a phrase mean nothing there and were printed
		// literally, so they are stripped.
		if (plg::Log) {
			plg::Log(StripColorTags(buffer.data()), plg::Severity::Info, plg::source_location::current());
		}
		return;
	}
	if (!IsValidSlot(slot)) {
		return; // -2 / 64 / ...: not a client and not the server console, print nothing
	}
	if (!g_mapReady || __s2sdk_PrintToConsole == nullptr) {
		return;
	}
	try {
		s2sdk::PrintToConsole(slot, plg::string(buffer.data()));
	} catch (...) {
	}
}

// --- logging ---------------------------------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

} // namespace admin_system
