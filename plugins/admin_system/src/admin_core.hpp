#pragma once
// Engine-independent core of the "admin_system" plugin: configuration model, database schema,
// admin/group loading, flag -> permission expansion and punishments.
//
// No Plugify, s2sdk or engine headers here - only plgdb - so the whole module is unit-testable
// against SQLite and MySQL (tests/admin_core_tests.cpp). plugin.cpp (phase 2) owns the engine
// half: commands, menus, chat, player lookups; it feeds this core a PlayerIdentity and applies
// what the hooks report back (disconnect on a ban, chat messages, timers).
//
// Compatibility contract with the original Metamod plugin (ref/pisex/cs2-admin_system):
//   * The table layout, the queries and the computed values are those of database.cpp; the
//     deviations below are the only ones, and each is marked in the code with "PARITY:".
//     - The online unpunish check of the original was disabled by an inverted condition
//       (`if (iAdminID != -1) bUnpunishAll = true;`); the intended rule of unpunish_type = 0
//       (only the punishing admin, unless the caller has '@admin/unpunish_all' or is the
//       console) is implemented here and in the offline path alike.
//     - console (slot -1) never indexes the per-slot arrays of the original (which read
//       g_pAdmins[-1]).
//     - every slot starts with "no punishment" (-1); the original's arrays started at 0,
//       which made IsPlayerPunished() answer true before the first load.
//     - timestamps are int64 in memory (the original stored unix times in int32 arrays).
//   * Every statement is asynchronous (plgdb) and its effects are applied from the callback,
//     which plgdb only runs inside Poll() on the game thread; the only blocking statement is
//     CreateSchema(), which the plugin calls once at startup.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plgdb/db.hpp>

namespace admin_system {

// --- constants -----------------------------------------------------------------
inline constexpr int kMaxPlayers = 64;

// Punishment types, the RT_* macros of the original admin.h.
inline constexpr int32_t RT_BAN = 0;
inline constexpr int32_t RT_MUTE = 1;
inline constexpr int32_t RT_GAG = 2;
inline constexpr int32_t RT_SILENCE = 3;
inline constexpr int32_t kPunishmentTypes = 4;

// Expiry sentinels used by the in-memory state of the original: -1 = no punishment,
// 0 = permanent, > 0 = absolute unix timestamp.
inline constexpr int64_t kNoExpiry = -1;
inline constexpr int64_t kPermanent = 0;

inline constexpr std::string_view kDefaultPrefix = "as_";
inline constexpr std::string_view kConsoleSteamId = "0";

// --- config ---------------------------------------------------------------------
// One flag of the "flags" block of core.ini.
struct Flag {
	std::string name;
	std::vector<std::string> permissions; // the 'access' block, only entries set to 1
};

// One reason of the "times_reasons" block (time_reason_type = 1).
struct ReasonTimes {
	std::string reason;
	std::vector<std::pair<int, std::string>> times; // seconds -> label ('0' = forever)
};

// The core.ini model. The defaults are the values of the shipped config file
// (ref/pisex/live-configs-admin_system/core.ini); the file itself is parsed by the plugin with
// the pisexcompat KV parser, which fills a Config and hands it to SetConfig().
struct Config {
	std::string database_prefix = std::string(kDefaultPrefix);
	int admin_server_id = -1;
	int punish_server_id = -1;
	bool punish_ip = true;
	int time_reason_type = 1;
	int ban_delay = 0;
	int notify_type = 1;
	int unpunish_type = 0;
	int immunity_type = 1;
	int punish_offline_count = 30;
	int unpunish_offline_count = 30;
	bool static_name = true;
	int message_type = 1;
	bool own_reason = true;
	bool mute_ignore_commands = true;

	std::vector<std::string> default_permissions;          // "default_permissions" block (value 1)
	std::map<std::string, Flag, std::less<>> flags;        // "flags" block
	std::vector<std::string> reasons[kPunishmentTypes];    // "reasons" block (time_reason_type = 0)
	std::vector<std::pair<int, std::string>> times[kPunishmentTypes];   // "times" block
	std::vector<ReasonTimes> reasons_times[kPunishmentTypes];           // "times_reasons" block

	// The prefix is interpolated into SQL (table names cannot be bound), so it must be a plain
	// identifier; anything else is rejected by SetConfig() (the default is kept).
	bool HasValidPrefix() const;

	const Flag* FindFlag(std::string_view flag) const;
	// GetPermissionsByFlag() of the original: the 'access' list of a flag, empty when unknown.
	std::vector<std::string> PermissionsByFlag(std::string_view flag) const;
	// GetFlagName() of the original.
	std::string FlagName(std::string_view flag) const;
	// The "default_permissions" block: permissions every player has.
	bool IsDefaultPermission(std::string_view permission) const;
};

// --- admin records ----------------------------------------------------------------
// g_pAdmins[slot] of the original.
struct AdminRecord {
	int64_t id = 0;                  // as_admins.id, 0 = the slot is not an admin
	uint64_t steamid = 0;            // steamid the record was loaded for
	std::string name;                // as_admins.name (or the player name when the column is NULL)
	int immunity = 0;
	int64_t expire_time = kNoExpiry; // 0 = permanent
	int64_t group_id = 0;
	std::string group_name;
	std::vector<std::string> flags;
	std::vector<std::string> permissions; // already expanded through the 'flags' block

	bool IsAdmin() const { return !flags.empty(); } // IsAdmin() of the original

	void Clear() { *this = AdminRecord{}; }
};

// One row of the joined SELECT of CheckPermissions().
struct AdminRow {
	int64_t admin_id = 0;
	std::optional<std::string> name;              // NULL -> the player name
	std::optional<std::string> flags;             // as_admins_servers.flags
	std::optional<int> immunity;                  // as_admins_servers.immunity
	int64_t expires = 0;                          // as_admins_servers.expires
	std::optional<int64_t> group_id;              // as_admins_servers.group_id
	std::optional<std::string> group_name;
	std::optional<std::string> group_flags;
	std::optional<int> group_immunity;
	int64_t server_id = 0;                        // as_admins_servers.server_id
};

// Rebuilds the record exactly as the original did, including its quirks:
//   * the server immunity (column 3) is folded in before the server filter is applied, the
//     group immunity after it - so an admin row of another server still contributes immunity;
//   * expire_time keeps the largest value seen, and 0 (permanent) wins over everything but 0;
//   * flags and permissions accumulate over every row that passes the filter;
//   * name is overwritten by every row (NULL means "use the player name").
AdminRecord ComputeAdminRecord(const std::vector<AdminRow>& rows, const Config& config, std::string_view player_name);

// --- punishments ---------------------------------------------------------------------
// One row of the punishment SELECT (p.* plus the admin columns of the LEFT JOIN).
struct PunishmentRow {
	int64_t id = 0;
	std::optional<std::string> name;
	std::string steamid;
	std::string ip;
	int64_t admin_id = 0;
	int64_t created = 0;
	int64_t expires = 0;
	std::string reason;
	bool unpunish_null = true;
	int64_t server_id = 0;
	int64_t punish_type = 0;
	std::optional<std::string> admin_steamid; // a.steamid of the joined admin row
	std::optional<std::string> admin_name;
	int64_t admin_db_id = 0;                  // a.id of the joined admin row
};

// g_iPunishments[slot][type], g_szPunishReasons and g_iAdminPunish of the original.
struct Punishment {
	int64_t expires = kNoExpiry;
	std::string reason;
	uint64_t admin_steamid = 0; // steamid64 of the admin that issued it, 0 = console/unknown
};

// The "if (current != 0 && current < incoming) current = incoming;" of the original, kept
// verbatim: a permanent punishment (0) is never replaced by a temporary one and vice versa.
int64_t MergeExpiry(int64_t current, int64_t incoming);

// What applying a batch of punishment rows produced. |ban| is set when the player must be
// disconnected; |forced| says whether the rows came from a reload (which also reports every
// punishment through onPlayerPunish).
struct PunishmentLoadOutcome {
	bool found = false;
	bool ban = false;
	bool forced = false;
};

// Applies the rows of CheckPunishments()/CheckPunishmentsForce() to a slot. |punish| is written
// for every non-ban row that passed the filters; bans only set the |ban| flag (the original
// never stored them in the arrays, it disconnected instead).
PunishmentLoadOutcome ApplyPunishmentRows(const std::vector<PunishmentRow>& rows, const Config& config, int64_t now,
										  bool forced, std::array<Punishment, kPunishmentTypes>& out);

// --- player identity -----------------------------------------------------------------
// The engine half of a player, filled by the plugin (phase 2): the core never asks the engine.
struct PlayerIdentity {
	int slot = -1;
	uint64_t steamid = 0;
	std::string name;
	std::string ip; // "1.2.3.4", empty when unknown
};

// --- expiry ----------------------------------------------------------------------------
struct ExpiryOutcome {
	struct ExpiredPunishment {
		int slot = 0;
		int type = 0;
	};
	std::vector<ExpiredPunishment> punishments; // tell the player, fire OnPlayerUnpunish(slot, type, -1)
	std::vector<int> admins;                    // print "admin expired", the original cleared the record
};

// --- the core ---------------------------------------------------------------------------
class AdminCore {
public:
	explicit AdminCore(plgdb::Database& db);
	~AdminCore();

	AdminCore(const AdminCore&) = delete;
	AdminCore& operator=(const AdminCore&) = delete;

	// --- configuration ---------------------------------------------------------------
	void SetConfig(Config config);
	const Config& GetConfig() const { return m_config; }

	// --- schema ----------------------------------------------------------------------
	// Creates the four tables (identical to the original, with the configured prefix) and the
	// reserved Console row (id 1, steamid 0). Blocking: startup only. Throws std::runtime_error
	// when a statement fails, like perm_storage's EnsureSchema().
	void CreateSchema();

	// --- timestamps ------------------------------------------------------------------
	// Overridable clock so tests can force expiry without sleeping.
	void SetClock(std::function<int64_t()> clock);
	int64_t Now() const;

	// --- admin state -------------------------------------------------------------------
	// The loaded record of a slot, nullptr for anything outside [0, kMaxPlayers). Slot -1 is
	// the console: it has no record, and HasPermission/HasFlag/IsAdmin answer true for it.
	const AdminRecord* FindAdmin(int slot) const;
	// HasPermission()/HasFlag()/IsAdmin() of the original: slot -1 (the console) always passes,
	// "default_permissions" and '@admin/root' are honoured.
	bool HasPermission(int slot, std::string_view permission) const;
	bool HasFlag(int slot, std::string_view flag) const;
	bool IsAdmin(int slot) const;

	// AddPlayerLocalFlag/Permission/Immunity of the original. They change the in-memory record
	// only; note that the flag list and the permission list are independent, exactly as in the
	// original (adding a flag does not expand it into permissions).
	void AddLocalFlag(int slot, std::string_view flag);
	void RemoveLocalFlag(int slot, std::string_view flag);
	void AddLocalPermission(int slot, std::string_view permission);
	void RemoveLocalPermission(int slot, std::string_view permission);
	void AddLocalImmunity(int slot, int immunity);
	void RemoveLocalImmunity(int slot);

	// --- asynchronous loads -----------------------------------------------------------
	// CheckPermissions(): reads admins + admins_servers + groups for one steamid and applies the
	// result in Poll(); onAdminLoaded fires only when a row was found (OnAdminConnectedSend).
	// A load in flight when the slot is reset is discarded.
	void LoadAdmin(int slot, uint64_t steamid, std::string player_name);
	// Clears the admin record and the punishments of a slot (RunPunishments/ResetPunishments +
	// the flag wipe of OnClientDisconnect) and invalidates loads in flight.
	void ResetSlot(int slot);

	// CheckPunishments(): the punishments of a player that just connected. A ban only sets
	// outcome.ban (the caller disconnects immediately, the original used a plain
	// DisconnectClient here and never stored the ban).
	void LoadPunishments(int slot, uint64_t steamid, std::string ip);
	// CheckPunishmentsForce(): the mm_as_reload_punish path; the caller has already reset the
	// slot. Bans are reported through onPunishmentsLoaded(force = true, ban = true) so the
	// caller can use ban_delay, and every row fires onPlayerPunish.
	void ReloadPunishments(int slot, uint64_t steamid, std::string ip);

	// --- punishment state --------------------------------------------------------------
	bool IsPunished(int slot, int type) const;          // IsPlayerPunished()
	int64_t PunishmentExpires(int slot, int type) const; // GetPlayerPunishmentExpired()
	std::string_view PunishmentReason(int slot, int type) const;
	uint64_t PunishmentAdminSteamId(int slot, int type) const;
	// IsClientMutedOrSilenced()/IsClientGaggedOrSilenced() without the engine part (the caller
	// still has to check that the slot is a real, non-bot player and is in game).
	bool IsMutedOrSilenced(int slot) const;
	bool IsGaggedOrSilenced(int slot) const;
	// The per-second timer of the original: forgets expired punishments and admin rights.
	// The returned slots are the ones the plugin has to announce.
	ExpiryOutcome ExpireState();

	// --- punishments --------------------------------------------------------------------
	// AddPlayerPunishment(): veto through onPlayerPunishPre, INSERT (with the ip when punish_ip
	// is on), in-memory update, then onPlayerPunish. Returns false when a hook vetoed the
	// punishment or the identity is unusable. A ban is only reported (type == RT_BAN), the
	// caller disconnects after ban_delay.
	bool Punish(const PlayerIdentity& player, int type, int time, std::string reason, int admin_slot);
	// AddOfflinePlayerPunishment(): the same, without an ip column and without a slot.
	void PunishOffline(std::string steamid64, std::string name, int type, int time, std::string reason, int admin_slot);
	// TryAddPunishment(): the immunity rules of immunity_type in front of Punish(). Returns
	// false when the target is immune.
	bool TryPunish(const PlayerIdentity& player, int type, int time, std::string reason, int admin_slot);
	// TryAddOfflinePunishment(): the same for a player that is not connected - the target's
	// immunity is read from the database first.
	void TryPunishOffline(std::string steamid64, std::string name, int type, int time, std::string reason, int admin_slot);
	// The immunity rules alone (immunity_type 0..3 of core.ini). |admin_immunity| is the immunity
	// of the caller, |target_immunity| the one of the target; slot -1 (console) always passes.
	static bool CanPunish(int admin_slot, int admin_immunity, int target_immunity, int immunity_type);

	// RemovePlayerPunishment(): the unpunish_type rule in front of Unpunish().
	bool TryUnpunish(int slot, int type, int admin_slot);
	// The UPDATE itself (unpunish_admin_id) plus the in-memory clear and the notification.
	void Unpunish(int slot, int type, int admin_slot);
	// RemoveOfflinePlayerPunishment().
	void UnpunishOffline(std::string steamid64, int type, int admin_slot);
	// RemoveExpiresAdmins(): drops the admins_servers rows whose expires has passed.
	void RemoveExpiredAdmins();

	// --- admin and group maintenance -----------------------------------------------------
	// AddPlayerAdmin() of the original: reuse the as_admins row when the steamid exists, then
	// INSERT IGNORE the admins_servers row for this server; an online target gets its rights
	// reloaded. expire_seconds is a duration, 0 = permanent. bDB = false changes the in-memory
	// record of an online target only.
	void AddAdmin(std::string name, std::string steamid64, std::string flags, int immunity, int expire_seconds,
				  int64_t group_id, std::string comment, bool db = true);
	// RemovePlayerAdmin(): delete the admins_servers row of this server (and the as_admins row
	// when no other server still uses it); online players lose their rights at once.
	void RemoveAdmin(std::string steamid64, bool db = true);
	// AddGroup()/RemoveGroup() of the mm_add_group / mm_remove_group commands.
	void AddGroup(std::string name, std::string flags, int immunity, std::function<void(int64_t)> onInserted = {});
	void RemoveGroup(std::string identifier);

	// --- hooks (called on the game thread, from Poll() or from the public methods) --------
	// A successful admin load (OnAdminConnectedSend of the original).
	std::function<void(int slot, const AdminRecord& admin)> onAdminLoaded;
	// The punishments of a slot were (re)loaded; |ban| asks for a disconnect (immediately for a
	// connect, after ban_delay when |forced|).
	std::function<void(int slot, PunishmentLoadOutcome outcome)> onPunishmentsLoaded;
	std::function<void(int slot, int type, int time, const std::string& reason, int admin_slot)> onPlayerPunish;
	std::function<void(const std::string& steamid64, const std::string& name, int type, int time, const std::string& reason,
					   int admin_slot)>
		onOfflinePlayerPunish;
	std::function<void(int slot, int type, int admin_slot)> onPlayerUnpunish;
	std::function<void(const std::string& steamid64, int type, int admin_slot)> onOfflinePlayerUnpunish;
	// Veto hooks (OnPlayerPunishPre / OnOfflinePlayerPunishPre): true blocks the punishment.
	std::function<bool(int slot, int type, int time, const std::string& reason, int admin_slot)> onPlayerPunishPre;
	std::function<bool(const std::string& steamid64, const std::string& name, int type, int time, const std::string& reason,
					   int admin_slot)>
		onOfflinePlayerPunishPre;
	// Engine half of AddAdmin/RemoveAdmin: the slots that currently have a steamid. Set by the
	// plugin; the core stays engine-free.
	std::function<std::vector<int>(uint64_t steamid)> findOnlineSlots;
	// SQL failures, bad rows and rejected values. Never empty in the plugin.
	std::function<void(std::string_view)> onError;

	// The prefixed table names, exposed for the plugin and the tests.
	std::string TableGroups() const;
	std::string TableAdmins() const;
	std::string TableAdminsServers() const;
	std::string TablePunishments() const;

private:
	struct SlotState {
		uint64_t generation = 1;
	};

	void ReportError(std::string_view message) const;
	bool SlotInRange(int slot) const;
	SlotState* FindSlot(int slot);
	const SlotState* FindSlot(int slot) const;
	Punishment* FindPunishment(int slot, int type);
	const Punishment* FindPunishment(int slot, int type) const;
	void ResetPunishments(int slot);
	// The INSERT IGNORE of the original, then a reload of the rights of the online slots.
	void AddAdminServerRow(int64_t admin_id, const std::string& steamid64, const std::string& flags, int immunity,
						   int64_t expires, int64_t group_id, const std::string& name, const std::vector<int>& slots);
	// as_admins.id of the caller, 1 (Console) for slot -1 - the admin_id column of the original.
	int64_t AdminDbId(int slot) const;
	uint64_t AdminSteamId(int slot) const;
	// The unpunish_type rule shared by the online and the offline path.
	bool MayUnpunish(int admin_slot, uint64_t punishment_admin_steamid) const;

	void Query(std::string sql, plgdb::Params params, plgdb::Callback callback);

	plgdb::Database& m_db;
	Config m_config;
	std::function<int64_t()> m_clock;
	std::array<SlotState, kMaxPlayers> m_slots;
	std::array<AdminRecord, kMaxPlayers> m_admins;
	std::array<std::array<Punishment, kPunishmentTypes>, kMaxPlayers> m_punishments;
	// Kept alive by the core; the callbacks that plgdb runs from Poll() hold a weak reference so
	// a result landing after the core is destroyed is dropped instead of touching freed state.
	std::shared_ptr<void> m_life;
};

} // namespace admin_system
