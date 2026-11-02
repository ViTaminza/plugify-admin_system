#pragma once
// Engine-independent core of the "admin_system" plugin: configuration model, database schema,
// admin/group loading, flag -> permission expansion and punishments.
//
// No Plugify, s2sdk or engine headers here - only plgdb - so the whole module is unit-testable
// against SQLite and MySQL (tests/admin_core_tests.cpp). plugin.cpp (phase 2) owns the engine
// half: commands, menus, chat, player lookups; it feeds this core a PlayerIdentity and applies
// what the hooks report back (disconnect on a ban, chat messages, timers).
//
// Compatibility contract with the original Metamod plugin (cs2-admin_system):
//   * The table layout, the queries and the computed values are those of database.cpp; the
//     deviations below are the only ones, and each is marked in the code with "PARITY:".
//     - The online unpunish check of the original was disabled by an inverted condition (it
//       went through the unpunish-all path whenever the caller passed an admin id); the
//       intended rule of unpunish_type = 0 (only the punishing admin, unless the caller has
//       '@admin/unpunish_all' or is the console) is implemented here and in the offline path
//       alike.
//     - console (slot -1) never indexes the per-slot arrays of the original (which read
//       g_pAdmins[-1]).
//     - every slot starts with "no punishment" (-1); the original's arrays started at 0,
//       which made IsPlayerPunished() answer true before the first load.
//     - timestamps are int64 in memory (the original stored unix times in int32 arrays).
//   * Every statement is asynchronous (plgdb) and its effects are applied from the callback,
//     which plgdb only runs inside Poll() on the game thread; the only blocking statement is
//     CreateSchema(), which the plugin calls once at startup.

#include <algorithm>
#include <array>
#include <chrono>
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

// --- per-slot admin load state ------------------------------------------------------
// The admin record of a player is read asynchronously, and "not loaded yet" is indistinguishable
// from "not an admin" through IsAdmin()/HasPermission(): both answer false. The state below is the
// missing completion signal (see LoadSnapshot()/IsAdminDataReady()):
//   * Unknown - no load was requested for the current slot generation (plugin start, ResetSlot,
//     a slot that was reclaimed, or a destroyed/not-ready core);
//   * Pending - the SELECT was queued and has not answered yet;
//   * Ready   - the last SELECT answered: the in-memory record is authoritative for load_steamid,
//     including the "no rows" answer (a definitive "this player is not an admin");
//   * Failed  - the SELECT failed (a plgdb error); the record is NOT authoritative and a retry is
//     expected. Like every other admin_core statement its effects only appear from Poll().
enum class AdminLoadState : int32_t {
	Unknown = 0,
	Pending = 1,
	Ready = 2,
	Failed = 3,
};

// One atomic view of a slot's load state. generation changes whenever the slot is reset/reused
// (ResetSlot), request is the id of the newest LoadAdmin() call: a consumer that saw an older pair
// (or a different steamid) must re-read instead of trusting the previous answer.
struct AdminLoadSnapshot {
	AdminLoadState state = AdminLoadState::Unknown;
	uint64_t steamid = 0;    // the steamid the state was requested for
	uint64_t generation = 0; // the slot generation the request belongs to
	uint64_t request = 0;    // the newest LoadAdmin() request id

	// True only when the answer is authoritative for the loaded steamid. It says nothing about
	// whether that player is an admin - IsAdmin()/HasPermission() answer that afterwards.
	bool IsAuthoritative() const { return state == AdminLoadState::Ready; }
};

// --- the retry policy of the plugin's 1 Hz reconcile -------------------------------
// The admin and punishment loads are queued from several places: the connect path, the reconcile
// loop and the reload/admin/punish commands plus AddAdmin(), which call LoadAdmin() /
// ReloadPunishments() directly. The reconcile must not duplicate a statement that is already in
// flight (that would supersede it by request id and throw the answer away), and it must retry a
// statement that never answered with a backoff. The decision and its bookkeeping are engine-free
// so the reconcile behaviour is unit-tested without a plugin.
inline constexpr std::chrono::seconds kLoadRetryInitial{10}; // first retry window
inline constexpr std::chrono::seconds kLoadRetryMax{60};     // backoff cap
// While the plgdb worker already has more than this many statements queued (a database outage
// makes each one sit in the connect timeout), nothing is queued or armed: the backlog must not
// grow.
inline constexpr std::size_t kMaxPendingLoads = 64;

struct SlotRetryPolicy {
	std::chrono::seconds initial = kLoadRetryInitial;
	std::chrono::seconds maximum = kLoadRetryMax;
	std::size_t maxPendingQueue = kMaxPendingLoads;
};

enum class SlotRetryAction : int32_t {
	Settled = 0,             // both statements answered: reset retryDelay/retryAt
	Wait = 1,                // nothing to do now
	ArmDeadline = 2,         // a load is already in flight and nothing is armed: arm, queue nothing
	ReloadPunishments = 3,   // the punishment SELECT timed out
	ReloadAdmin = 4,         // the admin SELECT timed out
	ReloadBoth = 5,          // both timed out
};

struct SlotRetryDecision {
	SlotRetryAction action = SlotRetryAction::Wait;
	// ArmDeadline: the window to arm. Reload*: the next backoff step to store and arm.
	std::chrono::seconds delay{0};
};

// Decide what the reconcile has to do for one tracked slot. |punishmentsLoaded| is the plugin's
// completion bit for the punishment load, |admin| the core's snapshot for the same slot,
// |steamid| the steamid the slot currently belongs to, |retryAt|/|currentDelay| the plugin's
// backoff bookkeeping (an unset retryAt is the epoch). Two facts keep a Pending statement safe:
//   * a request that is in flight from *outside* the reconcile (a reload command, AddAdmin) leaves
//     retryAt unset, so it only gets ArmDeadline - one full window, no duplicate SELECT;
//   * a statement that is still unanswered when its window expires is retried with the backoff,
//     and only if it is still missing (punishments are never replayed while punishmentsLoaded).
// The accepted residual risk: a punishment SELECT that is genuinely still queued when its window
// expires is re-queued by the retry (that is how a statement stuck behind a connect timeout is
// recovered). It re-applies the same rows, and for a ban row it can fire the ban/kick callback a
// second time; the reconcile only ever re-queues the punishment query when punishmentsLoaded is
// false, so this cannot happen for a slot whose punishment answer already arrived.
inline SlotRetryDecision DecideSlotRetry(bool punishmentsLoaded, const AdminLoadSnapshot& admin, uint64_t steamid,
										 std::chrono::steady_clock::time_point now,
										 std::chrono::steady_clock::time_point retryAt,
										 std::chrono::seconds currentDelay, std::size_t pendingQueue,
										 const SlotRetryPolicy& policy = {}) {
	const bool adminSettled = admin.state == AdminLoadState::Ready && admin.steamid == steamid;
	if (punishmentsLoaded && adminSettled) {
		return {SlotRetryAction::Settled, std::chrono::seconds(0)};
	}
	if (pendingQueue > policy.maxPendingQueue) {
		return {}; // the worker is saturated: neither queue nor arm anything
	}
	if (retryAt == std::chrono::steady_clock::time_point{}) {
		// Nothing is armed: a statement is probably in flight from outside this loop. Give it one
		// full window; only a load that is still unanswered when it expires is retried.
		return {SlotRetryAction::ArmDeadline, policy.initial};
	}
	if (now < retryAt) {
		return {};
	}
	const std::chrono::seconds next =
		currentDelay == std::chrono::seconds(0) ? policy.initial : std::min(currentDelay * 2, policy.maximum);
	SlotRetryDecision decision;
	decision.delay = next;
	decision.action = (!punishmentsLoaded && !adminSettled)
						  ? SlotRetryAction::ReloadBoth
						  : (!punishmentsLoaded ? SlotRetryAction::ReloadPunishments : SlotRetryAction::ReloadAdmin);
	return decision;
}

// Applies a decision to the plugin's two bookkeeping values. Shared by the reconcile loop and the
// tests, so a test cannot drift from the code it is supposed to cover.
inline void ApplySlotRetryDecision(const SlotRetryDecision& decision, std::chrono::steady_clock::time_point now,
								   std::chrono::steady_clock::time_point& retryAt, std::chrono::seconds& retryDelay) {
	switch (decision.action) {
	case SlotRetryAction::Settled:
		retryDelay = std::chrono::seconds(0);
		retryAt = {};
		break;
	case SlotRetryAction::Wait:
		break;
	case SlotRetryAction::ArmDeadline:
		retryAt = now + decision.delay;
		break;
	default:
		retryDelay = decision.delay;
		retryAt = now + decision.delay;
		break;
	}
}

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

// The core.ini model. The defaults are the values of the shipped config file; the file itself is
// parsed by the plugin with its own KV1 reader (src/kv1.hpp + src/admin_config.hpp, engine-free),
// which fills a Config and hands it to SetConfig().
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

// The merge rule of the original, kept verbatim: a permanent punishment (0) is never replaced
// by a temporary one, and vice versa.
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

	// --- admin load readiness ------------------------------------------------------------
	// The state of the asynchronous admin load of a slot; Unknown outside [0, kMaxPlayers).
	AdminLoadSnapshot LoadSnapshot(int slot) const;
	// The completion check a consumer needs, atomically: a slot in range whose admin SELECT
	// answered for exactly this steamid. False for Unknown/Pending/Failed, for a steamid the
	// current answer was not computed for (a reclaimed slot, another player), for the console and
	// out-of-range slots, and for steamid 0 (identity unknown). A Ready answer for a steamid that
	// has no admin row is still "ready": it is the definitive "not an admin", so a consumer must
	// call IsAdmin()/HasPermission() afterwards - it must never treat a false as "not an admin".
	bool IsAdminDataReady(int slot, uint64_t steamid) const;

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
	// The slot's load state goes Pending -> Ready (a row or no row) or Pending -> Failed; a load
	// in flight when the slot is reset (or superseded by a newer LoadAdmin of the same slot) is
	// discarded by its generation/request pair.
	void LoadAdmin(int slot, uint64_t steamid, std::string player_name);
	// Clears the admin record and the punishments of a slot (RunPunishments/ResetPunishments +
	// the flag wipe of OnClientDisconnect) and invalidates loads in flight; the load state goes
	// back to Unknown for the new slot generation.
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
	// |onComplete| (optional) is told whether the INSERT was stored: false for a veto, a
	// rejected value or a database error; it runs from Poll() for the stored case.
	void PunishOffline(std::string steamid64, std::string name, int type, int time, std::string reason, int admin_slot,
					   std::function<void(bool)> onComplete = {});
	// TryAddPunishment(): the immunity rules of immunity_type in front of Punish(). Returns
	// false when the target is immune.
	bool TryPunish(const PlayerIdentity& player, int type, int time, std::string reason, int admin_slot);
	// TryAddOfflinePunishment(): the same for a player that is not connected - the target's
	// immunity is read from the database first. |onComplete| sees the result of the INSERT.
	void TryPunishOffline(std::string steamid64, std::string name, int type, int time, std::string reason, int admin_slot,
						  std::function<void(bool)> onComplete = {});
	// The immunity rules alone (immunity_type 0..3 of core.ini). |admin_immunity| is the immunity
	// of the caller, |target_immunity| the one of the target; slot -1 (console) always passes.
	static bool CanPunish(int admin_slot, int admin_immunity, int target_immunity, int immunity_type);

	// RemovePlayerPunishment(): the unpunish_type rule in front of Unpunish().
	bool TryUnpunish(int slot, int type, int admin_slot);
	// The UPDATE itself (unpunish_admin_id), the in-memory clear and - from the UPDATE callback,
	// only when it actually lifted a row - the notification.
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
	// A successful admin load (OnAdminConnectedSend of the original). It runs after the record has
	// been applied and the slot's load state is Ready, so a listener may query the record, the
	// load state or call IsAdmin()/HasPermission() from inside it.
	std::function<void(int slot, const AdminRecord& admin)> onAdminLoaded;
	// Called with the parsed rows of a non-empty admin SELECT, after parsing and before the record
	// is built/applied. It is a seam for the exception path of LoadAdmin: a throw from here is
	// handled exactly like a parse failure (the slot goes Failed, never Ready with a cleared
	// record), which the tests use to drive that path without a real allocation failure.
	std::function<void(const std::vector<AdminRow>& rows)> onAdminRowsParsed;
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
		uint64_t generation = 1;          // bumped by ResetSlot(): the slot changed hands
		AdminLoadState load = AdminLoadState::Unknown;
		uint64_t load_steamid = 0;        // the steamid the current state belongs to
		uint64_t request = 0;             // id of the newest LoadAdmin() (stale answers are dropped)
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
