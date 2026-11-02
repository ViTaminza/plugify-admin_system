// admin_system — the plugin's own s2sdk safety guards (PITFALLS A1/A7, B1/B2, C1/C2, C9, C11).
//
// The plugin calls s2sdk directly (docs/PLUGIN_STANDARD.md §7: no pisexcompat), so every rule the
// old wrappers enforced lives here:
//
//   * Map gate (B1/B2). No client, entity or schema call is made before the first map and none in
//     the window from OnMapEnd until the next OnMapStart: the client list is torn down there and
//     s2sdk's client lookups read an emptied list, while the serializer map a schema lookup walks
//     does not exist before the first map. InitMapGate() registers the s2sdk OnMapStart/OnMapEnd
//     listeners (OnPluginStart) and refuses to register anything unless both Register *and*
//     Unregister bindings are present (A7); PollMapGate() latches readiness for a plugin loaded
//     into a running map - on the FIRST OnPluginUpdate only, only when no OnMapEnd was seen and
//     only when s2sdk reports a real map name ("", "<empty>" and a missing binding all keep the
//     gate shut); OnMapEnd closes it again and calls the plugin's map-end hook, which may only drop
//     state. ShutdownMapGate() removes the listeners (OnPluginEnd, A7).
//   * Client checks (C2). s2sdk exports do not validate their arguments, so HasClient() layers the
//     map gate, the slot range, IsClientConnected and (for the name methods) a controller entity;
//     HasController() adds the controller pointer *and* a usable, valid handle; HasPawn() adds the
//     pawn. The name/steamid/ip answers mirror pisexcompat: a slot without a client answers
//     0 / "" (never a stale value), "<unknown>" counts as an empty name, and GetClientIp is never
//     called for a bot (its net channel is null).
//   * Entity handles (C1). An entity int is a CEntityHandle (index | serial << 15): 0 is the
//     world, -1 is INVALID_EHANDLE_INDEX, and a *valid* handle may be negative once the serial
//     counter passes 65535 - the numeric filter drops exactly 0 and -1, never "<= 0".
//   * Owner identity (the menu layer's stale-request guard). A menu action or a queued display is
//     not allowed to apply to whoever happens to occupy the slot when it runs: CaptureOwner()
//     records the SteamID64 and the controller handle (index | serial << 15, so it includes the
//     generation) while the map is up, and OwnerMatches() re-captures and compares both. No raw
//     entity pointer is kept across frames (PITFALLS C3).
//   * Chat output (the Pisex printers). The cs2-menus colour table is kept byte-identical, so the
//     production phrases show the same colours, and the server-console printer strips the tags
//     instead of printing the raw bytes (the original did the same split).
//   * Timers (C9). The plugin creates NO s2sdk timer: every periodic action (punishment/admin
//     expiry, the per-second reconcile, the database Poll()) runs from OnPluginUpdate, and the
//     ban-delay kick is a queued deadline checked there too.
//
// Schema offsets (C8): this plugin reads and writes no schema fields at all - it only talks to
// clients, menus, events and the database - so there is nothing to resolve or cache here.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// plg::Severity / plg::Log of the logging helpers below.
#include <plg/plugin.hpp>

namespace admin_system {

// The plugin's slot universe (the engine's max clients; admin_core::kMaxPlayers has the same
// value). Kept here because every guard below is a slot guard.
inline constexpr int32_t kMaxSlots = 64;
inline constexpr int32_t kPunishTypeCount = 4;
// The server console / RCON caller: exactly -1 (PITFALLS C11). -2 is NOT a server console and must
// never reach a permission check or the console printer.
inline constexpr int32_t kServerConsoleSlot = -1;

// --- map lifecycle ---------------------------------------------------------------------------

// A plugin map-lifecycle hook, called by the OnMapStart/OnMapEnd listeners after the gate state
// has been updated. Plain function pointer (s2sdk listeners are C function pointers anyway) and
// noexcept in practice: the map-end hook runs while the client list is being torn down and must
// only drop state.
using MapLifecycleCallback = void (*)();

// Installs (or clears) the plugin's per-map hooks; pass nullptr to drop them on unload.
void SetMapLifecycleCallbacks(MapLifecycleCallback onMapStart, MapLifecycleCallback onMapEnd);

// True when the four map-listener bindings (Register + Unregister for both events) are present.
// Nothing is registered without them, so a reload cannot leave a callback into the unloaded
// library (PITFALLS A7).
bool MapListenerBindingsUsable();

// Registers the OnMapStart/OnMapEnd listeners. Touches no client; safe in OnPluginStart.
void InitMapGate();

// Per-frame pump (OnPluginUpdate): opens the gate for a plugin that loaded into a running map.
void PollMapGate();

// Removes the map listeners and closes the gate; called from OnPluginEnd so `plg reload` leaves
// nothing pointing into the unloaded library (PITFALLS A7).
void ShutdownMapGate();

// True while a map is up and client/entity calls are safe.
bool IsMapReady();

// --- slots and clients (PITFALLS C1/C2) ------------------------------------------------------

bool IsSlotInRange(int32_t slot);

// The slot has a client object and is connected (bots included).
bool HasClient(int32_t slot);
// ... and its controller entity exists with a live pointer and a usable handle.
bool HasController(int32_t slot);
// ... and the controller currently owns a pawn.
bool HasPawn(int32_t slot);

bool IsFakeClientSlot(int32_t slot);
bool IsAuthorizedSlot(int32_t slot);

// 0 when the slot has no connected client (or no map is running).
uint64_t ClientSteamID64(int32_t slot);
// Empty when the slot has no controller; s2sdk's "<unknown>" placeholder counts as empty.
std::string ClientName(int32_t slot);
// Empty for bots and for a slot without a client.
std::string ClientIp(int32_t slot);
void SetClientNameSafe(int32_t slot, const std::string& name);
// Disconnects the slot (Player::Kick); false when the binding is missing or the slot is invalid.
bool KickClientSlot(int32_t slot, int32_t reason);

// A usable (non-world, non-invalid, live) entity handle, only while a map is up: IsValidEntHandle
// walks the entity system, which is null before the first map (PITFALLS B1). The numeric filter
// drops 0 and -1 only: a valid handle can be negative once the serial counter passes 65535.
bool IsUsableEntityHandle(int32_t handle);

// --- owner identity (stale menu request guard) ------------------------------------------------

struct OwnerIdentity {
	int32_t slot = -1;
	uint64_t steamId = 0;
	int32_t controllerHandle = 0;

	constexpr bool IsSet() const {
		return slot >= 0 && steamId != 0 && controllerHandle != 0 && controllerHandle != -1;
	}
};

// Records the slot's verified owner. false (and out cleared) when there is no live map, no
// controller, the s2sdk SteamID binding is missing or the engine answers 0 - the caller must drop
// the request rather than apply it to an unverified player.
bool CaptureOwner(int32_t slot, OwnerIdentity& out);

// Re-captures the slot and compares SteamID64 *and* controller handle/generation with `owner`.
bool OwnerMatches(const OwnerIdentity& owner);

// --- output (the Pisex printer split) ---------------------------------------------------------

// Replaces {TAG} colour tags with the cs2-menus chat colour bytes (byte-identical table).
std::string Colorize(std::string_view text);
// The same tags removed ("\\n" becomes a real newline): for the server console.
std::string StripColorTags(std::string_view text);

// printf into a std::string (the shared formatter behind the printers), truncated at 2047 bytes.
std::string FormatText(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Pisex's PrintToChat: colour tags expanded, one client, no-op while no map is up.
void PrintChat(int32_t slot, const char* format, ...) __attribute__((format(printf, 2, 3)));
void PrintChatAll(const char* format, ...) __attribute__((format(printf, 1, 2)));
// Pisex's PrintToConsole: the server console is exactly slot -1 (tags stripped). Any other slot
// outside [0, kMaxSlots) is a programming error and prints nothing at all - a -2 must not become a
// server-console message.
void PrintConsole(int32_t slot, const char* format, ...) __attribute__((format(printf, 2, 3)));

// --- logging ---------------------------------------------------------------------------------

void LogMessage(std::string_view message, plg::Severity severity);
void LogInfo(std::string_view message);
void LogError(std::string_view message);

} // namespace admin_system
