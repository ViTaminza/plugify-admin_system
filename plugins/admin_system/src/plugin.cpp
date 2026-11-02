// admin_system — engine side: lifecycle, configuration, s2sdk listeners, the once-per-second
// reconcile/expiry pump, the admin_core hook bodies, the punishment notifications of the original
// and the 60 exported methods (the IAdminApi of the Pisex cs2-admin_system plugin).
//
// Ported from the original cs2-admin_system (admin_system.cpp, database.cpp, menus.cpp) onto
// native s2sdk + plgdb (docs/PLUGIN_STANDARD.md §7: no pisexcompat). Commands live in
// commands.cpp, the menu in menus.cpp, the s2sdk guards in admin_guards.cpp and the menu layer in
// admin_menus.cpp.
//
// Rules this file follows:
//   * nothing here blocks on the database except the startup path (CreateSchema in EnsureCore);
//   * every s2sdk/plugify callback catches its exceptions (a throw into the module would kill the
//     server, not the plugin);
//   * no client query before a map exists and none in the OnMapEnd window - admin_guards.cpp owns
//     that gate (PITFALLS B1/B2) and every client access below goes through it;
//   * the plugin never returns Failed from OnPluginStart (that stops every plugify plugin,
//     PITFALLS A1); a missing binding or a broken config leaves it running as a no-op core;
//   * no s2sdk timer is created (PITFALLS C9): the one-second expiry/reconcile work, the database
//     Poll() and the ban-delay kicks all run from OnPluginUpdate;
//   * OnPluginEnd drops every registration symmetrically (PITFALLS A7): the client listeners, the
//     chat listeners, the console commands, the static-name command listener/game event, every
//     menu (A7_UNRESOLVED when one cannot be removed safely) and the map listeners.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <admin_system/admin_system.hpp>

#include "admin_config.hpp"
#include "admin_guards.hpp"
#include "admin_menus.hpp"
#include "admin_plugin.hpp"

namespace admin_system {

AdminPlugin g_adminPlugin;

// --- small helpers ---------------------------------------------------------------------

std::string ToString(std::string_view view) {
	return std::string(view);
}

std::vector<std::string> ParsePipeList(std::string_view text) {
	std::vector<std::string> parts;
	std::size_t start = 0;
	while (start <= text.size()) {
		const std::size_t pos = text.find('|', start);
		const std::size_t end = pos == std::string_view::npos ? text.size() : pos;
		if (end > start) {
			parts.emplace_back(text.substr(start, end - start));
		}
		if (pos == std::string_view::npos) {
			break;
		}
		start = pos + 1;
	}
	return parts;
}

namespace {

constexpr std::size_t kMaxOfflineUsers = 256;
constexpr auto kReconcileInterval = std::chrono::seconds(1);
constexpr std::string_view kDefaultDataFile = "admin_system.sqlite3";

std::string ToString(const plg::string& text) {
	return std::string(std::string_view(text));
}

// A registry is keyed by the callback pointer (the original keyed listeners by plugin id; every
// consumer plugin registers exactly one function per event, and registering the same pointer
// again is a no-op, so a vector with duplicate rejection is the same thing).
template <typename Callback>
void AddListener(std::vector<Callback>& list, Callback callback) {
	if (callback == nullptr) {
		return;
	}
	if (std::find(list.begin(), list.end(), callback) == list.end()) {
		list.push_back(callback);
	}
}

template <typename Callback>
void RemoveListener(std::vector<Callback>& list, Callback callback) {
	list.erase(std::remove(list.begin(), list.end(), callback), list.end());
}

void OnClientAuthenticatedListener(int32_t playerSlot, uint64_t steamid);
void OnClientDisconnectListener(int32_t playerSlot, s2sdk::NetworkDisconnectionReason reason);
s2sdk::ResultType OnStaticNameCommandListener(int32_t caller, s2sdk::ConCommandContext context,
											  const plg::vector<plg::string>& args);
s2sdk::ResultType OnRoundStartEvent(const plg::string& name, void* event, bool dontBroadcast);
s2sdk::ResultType OnSayListener(int32_t caller, s2sdk::ConCommandContext context,
								const plg::vector<plg::string>& args);

// The map gate hooks (admin_guards.cpp calls them from the s2sdk listeners / the mid-map latch).
// Named OnMapGate* so they cannot be confused with the AdminPlugin member functions of the same
// meaning.
void OnMapGateStart();
void OnMapGateEnd();

// The menu layer's eligibility hook: the owner is unchanged AND the admin data of the slot is
// authoritative for that steamid.
bool MenuOwnerEligibleTrampoline(void* user, const OwnerIdentity& owner);

// The config/data directories of this plugin (empty when the host did not provide them).
std::string ConfigsDir() {
	if (!plg::GetConfigsDir) {
		return {};
	}
	return std::string(std::string_view(plg::GetConfigsDir()));
}

std::string DataDir() {
	if (!plg::GetDataDir) {
		return {};
	}
	return std::string(std::string_view(plg::GetDataDir()));
}

// s2sdk's ServerLanguage (the fallback of the language chain, core.cfg wins).
std::string ServerLanguage() {
	if (__s2sdk_GetServerLanguage == nullptr) {
		return {};
	}
	try {
		const plg::string language = s2sdk::GetServerLanguage();
		return std::string(std::string_view(language));
	} catch (...) {
		return {};
	}
}

} // namespace

// --- configuration ---------------------------------------------------------------------

bool AdminPlugin::LoadCoreConfig() {
	// LoadConfig() of the original, key for key (the file parser is engine-free: admin_config.hpp).
	// A missing or malformed file leaves the built-in defaults in place, exactly like the original's
	// failed LoadFromFile did (the Config defaults are the values of the shipped core.ini).
	Config config;
	std::string usedPath;
	const bool loaded = LoadCoreConfigFile(ConfigCandidates(ConfigsDir(), DataDir(), kCoreIniFile), config, usedPath);
	m_config = std::move(config);
	if (!loaded) {
		LogError("admin_system: cannot read core.ini (" + usedPath + ") - keeping the built-in defaults");
	}
	return loaded;
}

bool AdminPlugin::LoadSortingIni() {
	m_sortCategories.clear();
	m_sortItems.clear();
	std::string usedPath;
	if (LoadSortingFile(ConfigCandidates(ConfigsDir(), DataDir(), kSortingIniFile), m_sortCategories, m_sortItems,
						usedPath)) {
		return true;
	}
	LogError("admin_system: cannot read sorting.ini (" + usedPath +
			 ") - the menus fall back to the registry order");
	return false;
}

bool AdminPlugin::LoadPhrases() {
	// The server's admin_system.phrases.txt holds the phrases of the core *and* of the modules
	// (the modules ask the core for their keys), exactly like on the Metamod server.
	m_language = PickLanguage(CoreConfigLanguage(ConfigsDir()), ServerLanguage());
	m_phrases.clear();
	std::string usedPath;
	std::optional<std::map<std::string, std::string, std::less<>>> phrases =
		LoadFirstPhrases(PhraseCandidates(ConfigsDir()), m_language, usedPath);
	if (!phrases) {
		LogError("admin_system: cannot read admin_system.phrases.txt (" + usedPath +
				 ") - phrase keys are printed as-is");
		return false;
	}
	m_phrases = std::move(*phrases);
	return true;
}

const char* AdminPlugin::Phrase(const char* key) const {
	if (key == nullptr) {
		return "";
	}
	const auto found = m_phrases.find(key);
	return found == m_phrases.end() ? key : found->second.c_str();
}

// --- startup -------------------------------------------------------------------------

bool AdminPlugin::EnsureCore() {
	if (m_core != nullptr) {
		// Retry after a failed CreateSchema: the database and the core (with its hooks) are
		// already in place, only the schema attempt is repeated.
		try {
			m_core->CreateSchema();
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: creating the tables failed: ") + ex.what());
			return false;
		}
		m_coreReady = true;
		return true;
	}

	// Database settings: the databases.cfg entry of the original (section "admin_system"), with
	// "driver" and "sqlite_path" as optional keys for a SQLite server. The candidate order is the
	// one the Pisex loader used (see admin_config.hpp); the plugin's own
	// <configs>/admin_system/databases.cfg wins over nothing else that exists.
	const std::string configsDir = ConfigsDir();
	const std::string dataDir = DataDir();
	plgdb::Config database;
	database.driver = plgdb::Driver::MySQL;
	std::string usedPath;
	const DatabaseSettings settings = LoadDatabaseSettings(DatabaseCandidates(configsDir, dataDir), usedPath);
	if (settings.found) {
		if (settings.sqlite) {
			database.driver = plgdb::Driver::SQLite;
			database.sqlitePath = ResolveSqlitePath(dataDir, settings.sqlitePath);
		}
		database.host = settings.host;
		database.port = static_cast<uint16_t>(settings.port);
		database.user = settings.user;
		database.password = settings.password;
		database.database = settings.database;
	} else {
		// Fail soft: a local SQLite file keeps the plugin usable without a databases.cfg.
		LogError("admin_system: no 'admin_system' entry in addons/configs/databases.cfg (looked at " + usedPath +
				 ") - falling back to a local SQLite database");
		database.driver = plgdb::Driver::SQLite;
		database.sqlitePath = ResolveSqlitePath(dataDir, std::string(kDefaultDataFile));
	}

	m_dbConfig = database;
	m_db = plgdb::Database::Create(database);
	if (m_db == nullptr) {
		LogError("admin_system: the database configuration is invalid - running without storage");
		return false;
	}
	m_core = std::make_unique<AdminCore>(*m_db);
	m_core->SetConfig(m_config);
	m_core->onError = [](std::string_view message) { g_adminPlugin.HandleCoreError(message); };
	m_core->onAdminLoaded = [](int slot, const AdminRecord& admin) { g_adminPlugin.HandleAdminLoaded(slot, admin); };
	m_core->onPunishmentsLoaded = [](int slot, PunishmentLoadOutcome outcome) {
		g_adminPlugin.HandlePunishmentsLoaded(slot, outcome);
	};
	m_core->onPlayerPunish = [](int slot, int type, int time, const std::string& reason, int adminRef) {
		g_adminPlugin.HandlePlayerPunish(slot, type, time, reason, adminRef);
	};
	m_core->onOfflinePlayerPunish = [](const std::string& steamid64, const std::string& name, int type, int time,
									   const std::string& reason, int adminRef) {
		g_adminPlugin.HandleOfflinePlayerPunish(steamid64, name, type, time, reason, adminRef);
	};
	m_core->onPlayerUnpunish = [](int slot, int type, int adminRef) {
		g_adminPlugin.HandlePlayerUnpunish(slot, type, adminRef);
	};
	m_core->onOfflinePlayerUnpunish = [](const std::string& steamid64, int type, int adminRef) {
		g_adminPlugin.HandleOfflinePlayerUnpunish(steamid64, type, adminRef);
	};
	m_core->onPlayerPunishPre = [](int slot, int type, int time, const std::string& reason, int adminRef) {
		for (PlayerPunishPreCallback callback : g_adminPlugin.PlayerPunishPreListeners()) {
			try {
				if (callback != nullptr && callback(slot, type, time, plg::string(reason), adminRef)) {
					return true;
				}
			} catch (const std::exception& ex) {
				LogError(std::string("admin_system: exception in an OnPlayerPunishPre listener: ") + ex.what());
			} catch (...) {
				LogError("admin_system: unknown exception in an OnPlayerPunishPre listener");
			}
		}
		return false;
	};
	m_core->onOfflinePlayerPunishPre = [](const std::string& steamid64, const std::string& name, int type, int time,
										  const std::string& reason, int adminRef) {
		for (OfflinePlayerPunishPreCallback callback : g_adminPlugin.OfflinePlayerPunishPreListeners()) {
			try {
				if (callback != nullptr &&
					callback(plg::string(steamid64), plg::string(name), type, time, plg::string(reason), adminRef)) {
					return true;
				}
			} catch (const std::exception& ex) {
				LogError(std::string("admin_system: exception in an OnOfflinePlayerPunishPre listener: ") + ex.what());
			} catch (...) {
				LogError("admin_system: unknown exception in an OnOfflinePlayerPunishPre listener");
			}
		}
		return false;
	};
	// The engine half of AddPlayerAdmin/RemovePlayerAdmin: which online slots carry a steamid.
	m_core->findOnlineSlots = [](uint64_t steamid) {
		std::vector<int> slots;
		if (steamid == 0) {
			return slots;
		}
		for (int slot = 0; slot < kMaxSlots; ++slot) {
			if (g_adminPlugin.SlotSteamId(slot) == steamid) {
				slots.push_back(slot);
			}
		}
		return slots;
	};

	try {
		m_core->CreateSchema();
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: creating the tables failed: ") + ex.what());
		return false;
	}
	m_coreReady = true;
	return true;
}

void AdminPlugin::RetryCore() {
	// The database was not reachable when the plugin started: CreateSchema is retried from the
	// per-second pump with the backoff below, so the core (commands, menus, modules waiting for
	// OnCoreLoaded) comes up without a manual `plg reload admin_system`.
	if (m_coreReady || m_coreProbePending || std::chrono::steady_clock::now() < m_coreRetryAt) {
		return;
	}
	if (m_db == nullptr) {
		// EnsureCore() creates m_db/m_core *before* its CreateSchema() call, so a failed schema
		// attempt always leaves both in place. Without them only the local database setup can
		// have failed (an invalid configuration or a thread-spawn error); rebuilding it is local
		// work, not a network operation.
		if (EnsureCore()) {
			LogInfo("admin_system: the database is available - the core is ready");
			RunStartupWork();
			return;
		}
		ScheduleCoreRetry();
		return;
	}
	// Probe asynchronously first: a blackholed MySQL keeps every statement in the connect
	// timeout on the worker thread, and CreateSchema() is a blocking QuerySync - calling it
	// from the game thread would freeze the server for ~10 s per attempt. Only a probe that
	// answered lets the game thread run the schema (the connection is then established, so it
	// completes quickly).
	m_coreProbePending = true;
	m_db->Query("SELECT 1", {}, [this](const plgdb::Result& result) {
		m_coreProbePending = false;
		if (!result.Ok()) {
			ScheduleCoreRetry();
			return;
		}
		if (EnsureCore()) {
			LogInfo("admin_system: the database is available - the core is ready");
			RunStartupWork();
			return;
		}
		ScheduleCoreRetry();
	});
}

void AdminPlugin::ScheduleCoreRetry() {
	m_coreRetryDelaySeconds = std::min(m_coreRetryDelaySeconds * 2, 60);
	m_coreRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(m_coreRetryDelaySeconds);
	LogError("admin_system: the core is still not ready - the next attempt is in " +
			 std::to_string(m_coreRetryDelaySeconds) + "s");
}

void AdminPlugin::RunStartupWork() {
	// StartupServer runs on the first update with a map; when the database was down at plugin
	// start the retry calls this as soon as the schema exists. m_coreLoaded keeps it once-only.
	if (m_core == nullptr || !m_coreReady || m_coreLoaded) {
		return;
	}
	Core().RemoveExpiredAdmins();
	BroadcastCoreLoaded();
	ReconcileSlots();
}

void AdminPlugin::RegisterStaticNameHooks() {
	// static_name = 1: the name of an admin is forced to the database name (the original hooked the
	// jointeam console command and the round_start event for this).
	if (!m_config.static_name) {
		return;
	}
	// "jointeam" is not a ConCommand in CS2 (the game parses it in its ClientCommand path), so
	// neither AddConsoleCommand (it would shadow the game command) nor a named listener works:
	// s2sdk runs the GLOBAL listeners for every client command, so one global listener dispatches
	// it by name (the same route pisexcompat used, PITFALLS C11).
	if (__s2sdk_AddCommandListener != nullptr && __s2sdk_RemoveCommandListener != nullptr) {
		try {
			if (s2sdk::AddCommandListener(plg::string(), &OnStaticNameCommandListener, s2sdk::HookMode::Pre)) {
				m_staticNameCommandRegistered = true;
			} else {
				LogError("admin_system: could not hook the client command dispatcher for the static name");
			}
		} catch (...) {
			LogError("admin_system: hooking the client command dispatcher failed");
		}
	} else {
		LogError("admin_system: AddCommandListener/RemoveCommandListener is unavailable - the static "
				 "name is not applied on jointeam");
	}
	// Both halves of the event pair are required before the hook (PITFALLS A7).
	if (__s2sdk_HookEvent != nullptr && __s2sdk_UnhookEvent != nullptr) {
		try {
			if (s2sdk::HookEvent(plg::string("round_start"), &OnRoundStartEvent, s2sdk::HookMode::Pre) ==
				s2sdk::EventHookError::Okay) {
				m_roundStartHooked = true;
			} else {
				LogError("admin_system: s2sdk refused the round_start hook for the static name");
			}
		} catch (...) {
			LogError("admin_system: hooking round_start failed");
		}
	} else {
		LogError("admin_system: HookEvent/UnhookEvent is unavailable - the static name is not applied "
				 "on round_start");
	}
}

void AdminPlugin::RegisterCoreMenus() {
	// The core's own category/items (the RegisterCategory/RegisterItem block of AllPluginsLoaded).
	RegisterCategory(plg::string("punishments"), plg::string("Category_Punishments"), nullptr);
	RegisterItem(plg::string("punish"), plg::string("Item_PunishPlayer"), plg::string("punishments"),
				 plg::string("@admin/ban|@admin/mute|@admin/gag|@admin/silence"), nullptr, &AdminSelectPunish);
	RegisterItem(plg::string("unpunish"), plg::string("Item_UnPunishPlayer"), plg::string("punishments"),
				 plg::string("@admin/unban|@admin/unmute|@admin/ungag|@admin/unsilence"), nullptr, &AdminSelectUnpunish);
	RegisterItem(plg::string("punish_offline"), plg::string("Item_PunishOfflinePlayer"), plg::string("punishments"),
				 plg::string("@admin/ban|@admin/mute|@admin/gag|@admin/silence"), nullptr, &AdminSelectPunishOffline);
	RegisterItem(plg::string("unpunish_offline"), plg::string("Item_UnPunishOfflinePlayer"), plg::string("punishments"),
				 plg::string("@admin/unban|@admin/unmute|@admin/ungag|@admin/unsilence"), nullptr,
				 &AdminSelectUnpunishOffline);
}

void AdminPlugin::BroadcastCoreLoaded() {
	m_coreLoaded = true;
	for (CoreLoadedCallback callback : m_onCoreLoaded) {
		try {
			if (callback != nullptr) {
				callback();
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in an OnCoreLoaded listener: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in an OnCoreLoaded listener");
		}
	}
}

// --- lifecycle --------------------------------------------------------------------------

plg::PluginResult AdminPlugin::OnPluginStart() {
	try {
		LoadCoreConfig();
		LoadPhrases();
		LoadSortingIni();

		if (!EnsureCore()) {
			// Fail soft: every handler is still registered below, the permission checks stay
			// fail-closed while the core is missing (commands.cpp) and the per-second pump retries
			// the schema. The API answers with neutral values until then.
			LogError("admin_system: the core is not ready - the plugin retries the database in the background");
			m_coreRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(m_coreRetryDelaySeconds);
		}

		// The map gate first: every client/entity call below is behind it (PITFALLS B1/B2).
		SetMapLifecycleCallbacks(&OnMapGateStart, &OnMapGateEnd);
		InitMapGate();

		// The s2sdk listeners of the original (authentication + disconnect) and the chat listeners
		// for gags/silences. Every registration is guarded: a missing binding must degrade to a
		// no-op, never to a call through a null function pointer, and both halves of every pair are
		// required before anything is registered (PITFALLS A7).
		if (__s2sdk_OnClientAuthenticated_Register != nullptr && __s2sdk_OnClientAuthenticated_Unregister != nullptr) {
			try {
				s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedListener);
				m_authenticatedListenerRegistered = true;
			} catch (...) {
				LogError("admin_system: registering the s2sdk OnClientAuthenticated listener failed - the "
						 "reconcile loop still loads admins and punishments");
			}
		} else {
			LogError("admin_system: the s2sdk OnClientAuthenticated listener is not available - admins and "
					 "punishments are only loaded by the reconcile loop");
		}
		if (__s2sdk_OnClientDisconnect_Register != nullptr && __s2sdk_OnClientDisconnect_Unregister != nullptr) {
			try {
				s2sdk::OnClientDisconnect_Register(&OnClientDisconnectListener);
				m_disconnectListenerRegistered = true;
			} catch (...) {
				LogError("admin_system: registering the s2sdk OnClientDisconnect listener failed - a slot is "
						 "dropped by the reconcile loop instead");
			}
		} else {
			LogError("admin_system: the s2sdk OnClientDisconnect listener is not available - a slot is dropped "
					 "by the reconcile loop instead");
		}
		if (__s2sdk_AddCommandListener != nullptr && __s2sdk_RemoveCommandListener != nullptr) {
			try {
				if (s2sdk::AddCommandListener(plg::string("say"), &OnSayListener, s2sdk::HookMode::Pre)) {
					m_sayListenerRegistered = true;
				} else {
					LogError("admin_system: could not listen on 'say' - the mute/gag chat block is not applied");
				}
			} catch (...) {
				LogError("admin_system: registering the 'say' listener failed - the mute/gag chat block is not "
						 "applied");
			}
			try {
				if (s2sdk::AddCommandListener(plg::string("say_team"), &OnSayListener, s2sdk::HookMode::Pre)) {
					m_sayTeamListenerRegistered = true;
				} else {
					LogError("admin_system: could not listen on 'say_team' - the mute/gag chat block is not applied");
				}
			} catch (...) {
				LogError("admin_system: registering the 'say_team' listener failed - the mute/gag chat block is "
						 "not applied");
			}
		} else {
			LogError("admin_system: AddCommandListener/RemoveCommandListener is unavailable - the mute/gag chat "
					 "block is not applied");
		}

		RegisterCommands();  // commands.cpp
		RegisterCoreMenus();
		RegisterStaticNameHooks();
		// The native menu layer: burns MenuId 0 when the full lifecycle bindings are present.
		StartMenuLayer(&g_adminPlugin, &MenuOwnerEligibleTrampoline);

		LogInfo("admin_system: plugin started (prefix '" + m_config.database_prefix + "', servers " +
				std::to_string(m_config.admin_server_id) + "/" + std::to_string(m_config.punish_server_id) +
				", language '" + m_language + "')");
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in OnPluginStart: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in OnPluginStart");
	}
	return plg::PluginResult{}; // never Failed (PITFALLS A1)
}

plg::PluginResult AdminPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		// Opens the gate on the first update after a mid-map load; after OnMapEnd only a new
		// OnMapStart opens it again (PITFALLS B1/B2).
		PollMapGate();
		// The database pump: completed statements run their callbacks here, on the game thread.
		// Every callback that could reach a client checks the map gate itself (admin_guards).
		if (m_db != nullptr) {
			m_db->Poll();
		}
		// Queued menu actions/displays and the ended menus. Drained even with the gate closed: the
		// closed branch only retries the safe destroy path (no client call).
		ProcessMenuRequests();
		if (!IsMapReady()) {
			return plg::PluginResult{};
		}
		// The original's StartupServer work, once both the map and the core are ready.
		RunStartupWork();
		// Ban-delay kicks (the original used a Pisex timer; PITFALLS C9 forbids s2sdk timers here).
		ProcessPendingKicks();
		// The one-second pump of the original: punishment/admin expiry, the reconcile and the
		// database retry. No s2sdk timer is involved.
		m_updateAccumulator += dt;
		if (m_updateAccumulator >= std::chrono::seconds(1)) {
			m_updateAccumulator = std::chrono::milliseconds(0);
			OnSecondTick();
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in OnPluginUpdate: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in OnPluginUpdate");
	}
	return plg::PluginResult{};
}

plg::PluginResult AdminPlugin::OnPluginEnd() {
	// Symmetric teardown (PITFALLS A7). Each step is guarded: the remaining registrations must
	// still be dropped even when one of them fails.
	if (m_authenticatedListenerRegistered) {
		m_authenticatedListenerRegistered = false;
		try {
			if (__s2sdk_OnClientAuthenticated_Unregister != nullptr) {
				s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedListener);
			}
		} catch (...) {
		}
	}
	if (m_disconnectListenerRegistered) {
		m_disconnectListenerRegistered = false;
		try {
			if (__s2sdk_OnClientDisconnect_Unregister != nullptr) {
				s2sdk::OnClientDisconnect_Unregister(&OnClientDisconnectListener);
			}
		} catch (...) {
		}
	}
	if (m_sayListenerRegistered) {
		m_sayListenerRegistered = false;
		try {
			if (__s2sdk_RemoveCommandListener != nullptr) {
				s2sdk::RemoveCommandListener(plg::string("say"), &OnSayListener, s2sdk::HookMode::Pre);
			}
		} catch (...) {
		}
	}
	if (m_sayTeamListenerRegistered) {
		m_sayTeamListenerRegistered = false;
		try {
			if (__s2sdk_RemoveCommandListener != nullptr) {
				s2sdk::RemoveCommandListener(plg::string("say_team"), &OnSayListener, s2sdk::HookMode::Pre);
			}
		} catch (...) {
		}
	}
	UnregisterCommands(); // every console command / command listener of commands.cpp
	if (m_staticNameCommandRegistered) {
		m_staticNameCommandRegistered = false;
		try {
			if (__s2sdk_RemoveCommandListener != nullptr) {
				s2sdk::RemoveCommandListener(plg::string(), &OnStaticNameCommandListener, s2sdk::HookMode::Pre);
			}
		} catch (...) {
		}
	}
	if (m_roundStartHooked) {
		m_roundStartHooked = false;
		try {
			if (__s2sdk_UnhookEvent != nullptr) {
				s2sdk::UnhookEvent(plg::string("round_start"), &OnRoundStartEvent, s2sdk::HookMode::Pre);
			}
		} catch (...) {
		}
	}
	// Destroy every menu while the map gate still tells the layer whether a backend close is safe.
	try {
		ShutdownMenuLayer();
	} catch (...) {
	}
	try {
		SetMapLifecycleCallbacks(nullptr, nullptr);
		ShutdownMapGate();
	} catch (...) {
	}
	// The core has to go before the database (its callbacks hold a lifetime token).
	m_core.reset();
	m_db.reset();
	m_coreLoaded = false;
	m_coreReady = false;
	m_coreProbePending = false;
	ClearPendingKicks();
	for (SlotState& state : m_slots) {
		state = SlotState{};
	}
	for (OwnReasonState& own : m_ownReasons) {
		own = OwnReasonState{};
	}
	for (std::string& category : m_lastCategory) {
		category.clear();
	}
	m_updateAccumulator = std::chrono::milliseconds(0);
	m_sinceReconcile = std::chrono::milliseconds(0);
	return plg::PluginResult{};
}

// --- map gate hooks ------------------------------------------------------------------------

void AdminPlugin::OnMapStartHook() {
	// A new map: the per-second pump starts from a clean accumulator (no client call here).
	m_updateAccumulator = std::chrono::milliseconds(0);
	m_sinceReconcile = std::chrono::milliseconds(0);
}

void AdminPlugin::OnMapEndHook() {
	// PITFALLS B2: from here until the next OnMapStart the client list is torn down - only state
	// is dropped, nothing is queried and nothing is printed.
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		ClearSlot(slot, "map end");
	}
	m_updateAccumulator = std::chrono::milliseconds(0);
	m_sinceReconcile = std::chrono::milliseconds(0);
	// Menus: cancel-confirmed teardown only (no client call with the map down, PITFALLS C10).
	CloseMenusOnMapEnd();
}

// --- engine side -------------------------------------------------------------------------

void AdminPlugin::HandleClientAuthenticated(int32_t playerSlot, uint64_t steamid) {
	if (m_core == nullptr || !m_coreReady || !IsMapReady() || playerSlot < 0 || playerSlot >= kMaxSlots) {
		return;
	}
	// The event documents "Steam account ID or 0 if not available" and can fire before the
	// client is fully connected (perm_connector guards it the same way): prefer the client's
	// canonical SteamID64, and let the reconcile load a slot whose id is still a 32-bit account
	// id - otherwise the wrong key is queried and the slot is reloaded a second later.
	const uint64_t canonical = ClientSteamID64(playerSlot);
	if (canonical != 0) {
		if (steamid != 0 && steamid != canonical) {
			LogMessage("admin_system: OnClientAuthenticated reported " + std::to_string(steamid) + " for slot " +
						   std::to_string(playerSlot) + ", but the client's SteamID64 is " +
						   std::to_string(canonical) + " - using the latter",
					   plg::Severity::Warning);
		}
		steamid = canonical;
	} else if (steamid < (1ull << 32)) {
		return; // 0 or a 32-bit account id: not a SteamID64, wait for the reconcile
	}
	if (steamid == 0) {
		return;
	}
	SlotState& state = m_slots[static_cast<size_t>(playerSlot)];
	if (state.requested && state.steamid == steamid) {
		return; // already requested (the reconcile may have been faster than the event)
	}
	state.steamid = steamid;
	state.requested = true;
	state.punishmentsLoaded = false;
	state.name = ClientName(playerSlot);
	state.retryDelay = std::chrono::seconds(0);
	state.retryAt = std::chrono::steady_clock::now() + kLoadRetryInitial;

	// CheckPunishments() + CheckPermissions() of the original's OnClientAuthorized/ClientConnect.
	m_core->ResetSlot(playerSlot);
	m_core->LoadPunishments(playerSlot, steamid, ClientIp(playerSlot));
	m_core->LoadAdmin(playerSlot, steamid, state.name);
}

void AdminPlugin::HandleClientDisconnect(int32_t playerSlot) {
	if (playerSlot < 0 || playerSlot >= kMaxSlots) {
		return;
	}
	// The original kept the last players around for the offline menus. Only the tracked state is
	// read here - the disconnect event fires while the client is still valid, but the map gate may
	// already be closed (changelevel), so no s2sdk client call is made.
	const SlotState& state = m_slots[static_cast<size_t>(playerSlot)];
	if (m_core != nullptr && state.steamid != 0) {
		OfflineUser user;
		user.steamid = state.steamid;
		user.name = state.name;
		for (int type = 0; type < kPunishTypeCount; ++type) {
			user.punished[static_cast<size_t>(type)] = m_core->IsPunished(playerSlot, type);
			user.admin[static_cast<size_t>(type)] = m_core->PunishmentAdminSteamId(playerSlot, type);
		}
		m_offlineUsers.push_back(std::move(user));
		const std::size_t cap = m_config.punish_offline_count > 0
									? std::min<std::size_t>(static_cast<std::size_t>(m_config.punish_offline_count),
															kMaxOfflineUsers)
									: kMaxOfflineUsers;
		while (m_offlineUsers.size() > cap) {
			m_offlineUsers.pop_front();
		}
	}
	ClearSlot(playerSlot, "disconnected");
}

void AdminPlugin::ClearSlot(int32_t playerSlot, const char* reason) {
	(void)reason;
	if (playerSlot < 0 || playerSlot >= kMaxSlots) {
		return;
	}
	m_slots[static_cast<size_t>(playerSlot)] = SlotState{};
	m_ownReasons[static_cast<size_t>(playerSlot)] = OwnReasonState{};
	m_lastCategory[static_cast<size_t>(playerSlot)].clear();
	if (m_core != nullptr) {
		m_core->ResetSlot(playerSlot);
	}
}

bool AdminPlugin::IsSlotHuman(int32_t slot) const {
	if (slot < 0 || slot >= kMaxSlots || !IsMapReady()) {
		return false;
	}
	return HasClient(slot) && IsAuthorizedSlot(slot) && !IsFakeClientSlot(slot);
}

uint64_t AdminPlugin::SlotSteamId(int32_t slot) const {
	if (slot < 0 || slot >= kMaxSlots) {
		return 0;
	}
	if (IsSlotHuman(slot)) {
		if (const uint64_t live = ClientSteamID64(slot); live != 0) {
			return live;
		}
	}
	return m_slots[static_cast<size_t>(slot)].steamid;
}

PlayerIdentity AdminPlugin::IdentityOf(int32_t slot) const {
	PlayerIdentity player;
	player.slot = slot;
	player.steamid = SlotSteamId(slot);
	if (slot >= 0 && slot < kMaxSlots) {
		player.name = ClientName(slot);
		if (player.name.empty()) {
			player.name = m_slots[static_cast<size_t>(slot)].name;
		}
		player.ip = ClientIp(slot);
	}
	return player;
}

void AdminPlugin::ReconcileSlots() {
	if (m_core == nullptr || !m_coreReady || !IsMapReady()) {
		return;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (!HasClient(slot)) {
				if (m_slots[static_cast<size_t>(slot)].steamid != 0) {
					// No disconnect event (or a reused slot): drop the tracked state.
					ClearSlot(slot, "gone");
				}
				continue;
			}
			if (!IsSlotHuman(slot)) {
				continue;
			}
			const uint64_t steamid = ClientSteamID64(slot);
			if (steamid == 0) {
				continue;
			}
			SlotState& state = m_slots[static_cast<size_t>(slot)];
			const auto now = std::chrono::steady_clock::now();
			if (state.requested && state.steamid == steamid) {
				// Punishments and admin rights are two separate statements with two separate
				// completion signals: the punishment load reports through onPunishmentsLoaded,
				// the admin load through the core's per-slot load state. The engine-free
				// DecideSlotRetry() (admin_core.hpp) owns the rule - it is unit-tested, so the
				// loop cannot drift from the covered behaviour:
				//   * both answered -> settle and reset the backoff;
				//   * a load in flight from outside the loop (a reload/add command calls
				//     LoadAdmin()/ReloadPunishments() directly) -> arm one full window, never a
				//     duplicate SELECT that would supersede the in-flight one by request id;
				//   * a load still unanswered when its window expires -> retry only the statement
				//     that is missing, with the 10/20/40/60 backoff;
				//   * the worker queue is saturated -> do nothing at all.
				const AdminLoadSnapshot adminLoad = m_core->LoadSnapshot(slot);
				const SlotRetryDecision decision =
					DecideSlotRetry(state.punishmentsLoaded, adminLoad, steamid, now, state.retryAt,
									state.retryDelay, m_db->Pending());
				ApplySlotRetryDecision(decision, now, state.retryAt, state.retryDelay);
				if (decision.action == SlotRetryAction::Settled ||
					decision.action == SlotRetryAction::Wait ||
					decision.action == SlotRetryAction::ArmDeadline) {
					// The clock change (settle reset / the freshly armed window) is already applied.
					continue;
				}
				LogError("admin_system: the load of slot " + std::to_string(slot) + " did not answer (" +
						 (decision.action == SlotRetryAction::ReloadAdmin
							  ? std::string("admin")
							  : decision.action == SlotRetryAction::ReloadPunishments ? std::string("punishments")
																					 : std::string("punishments/admin")) +
						 ") - requesting it again in " + std::to_string(state.retryDelay.count()) + "s");
				// No ResetSlot() here: the slot still belongs to the same player, and wiping its
				// admin rights and punishments on every retry would strip a live admin while the
				// database is unreachable.
				state.name = ClientName(slot);
				if (decision.action != SlotRetryAction::ReloadAdmin) {
					m_core->LoadPunishments(slot, steamid, ClientIp(slot));
				}
				if (decision.action != SlotRetryAction::ReloadPunishments) {
					m_core->LoadAdmin(slot, steamid, state.name);
				}
				continue;
			}
			// OnClientAuthenticated is not always delivered, so this is the reliable path - and
			// it also covers players that were already connected when the plugin was loaded.
			state.steamid = steamid;
			state.requested = true;
			state.punishmentsLoaded = false;
			state.name = ClientName(slot);
			state.retryDelay = std::chrono::seconds(0);
			state.retryAt = now + kLoadRetryInitial;
			m_core->ResetSlot(slot);
			m_core->LoadPunishments(slot, steamid, ClientIp(slot));
			m_core->LoadAdmin(slot, steamid, state.name);
		} catch (const std::exception& ex) {
			LogError("admin_system: reconcile of slot " + std::to_string(slot) + " failed: " + ex.what());
		} catch (...) {
			LogError("admin_system: reconcile of slot " + std::to_string(slot) + " failed");
		}
	}
}

void AdminPlugin::OnSecondTick() {
	if (!IsMapReady()) {
		return;
	}
	if (!m_coreReady) {
		// The database was down at plugin start (or a retry is pending): keep trying so the
		// core and its commands become usable without a manual reload.
		RetryCore();
		if (!m_coreReady) {
			return;
		}
	}
	// The per-second timer of the original: forget punishments and admin rights that expired.
	const ExpiryOutcome outcome = m_core->ExpireState();
	for (const ExpiryOutcome::ExpiredPunishment& expired : outcome.punishments) {
		PrintChat(expired.slot, "%s", Phrase("MuteExpired"));
		// PARITY: the original notified its listeners with admin id -1 for an expiry.
		HandlePlayerUnpunish(expired.slot, expired.type, -1);
	}
	for (const int slot : outcome.admins) {
		PrintChat(slot, "%s", Phrase("AdminExpired"));
	}

	m_sinceReconcile += std::chrono::seconds(1);
	if (m_sinceReconcile >= kReconcileInterval) {
		m_sinceReconcile = std::chrono::milliseconds(0);
		ReconcileSlots();
	}
}

bool AdminPlugin::OnChatPre(int32_t playerSlot, const char* content, bool team) {
	(void)team;
	try {
		if (playerSlot < 0 || playerSlot >= kMaxSlots || content == nullptr) {
			return true;
		}
		// The own-reason prompt of the punishment menu: the next chat line is the reason.
		OwnReasonState& own = m_ownReasons[static_cast<size_t>(playerSlot)];
		if (own.active) {
			std::string reason(content);
			if (!reason.empty() && (reason.front() == '"' || reason.front() == '\'')) {
				reason.erase(0, 1);
			}
			if (!reason.empty() && (reason.back() == '"' || reason.back() == '\'')) {
				reason.pop_back();
			}
			own.active = false;
			ShowTimesMenuFor(playerSlot, own.target, own.type, "own", own.offline, reason);
			return false;
		}
		if (m_core == nullptr || !m_core->IsGaggedOrSilenced(playerSlot)) {
			return true;
		}
		// PARITY (fix): the original tested content[1], which only matched a quoted message; the
		// documented meaning of mute_ignore_commands = 1 is "chat commands keep working".
		if (content[0] == '!' || content[0] == '/') {
			return !m_config.mute_ignore_commands;
		}
		const int type = m_core->IsPunished(playerSlot, RT_GAG) ? RT_GAG : RT_SILENCE;
		PrintChat(playerSlot, Phrase("MuteActiveChat"), CStr(std::string(m_core->PunishmentReason(playerSlot, type))),
				  CStr(FormatTime(m_core->PunishmentExpires(playerSlot, type))));
		return false;
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the chat listener: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the chat listener");
	}
	return true;
}

// --- admin_core hooks ---------------------------------------------------------------------

void AdminPlugin::HandleAdminLoaded(int32_t slot, const AdminRecord& admin) {
	// OnAdminConnectedSend of the original.
	for (AdminConnectedCallback callback : m_onAdminConnected) {
		try {
			if (callback != nullptr) {
				callback(slot);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in an OnAdminConnected listener: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in an OnAdminConnected listener");
		}
	}
	(void)admin;
}

void AdminPlugin::HandlePunishmentsLoaded(int32_t slot, const PunishmentLoadOutcome& outcome) {
	if (slot < 0 || slot >= kMaxSlots || m_core == nullptr) {
		return;
	}
	SlotState& state = m_slots[static_cast<size_t>(slot)];
	state.punishmentsLoaded = true;
	// The slot is settled only when BOTH statements answered: the punishment load (this callback)
	// and the admin load of the same steamid (the core's per-slot state). Until then the backoff
	// of the pending retry stays, so the admin-only retry keeps its 10/20/40/60 s spacing instead
	// of being re-armed every time a punishment load finishes.
	const AdminLoadSnapshot adminLoad = m_core->LoadSnapshot(slot);
	if (adminLoad.state == AdminLoadState::Ready && adminLoad.steamid == state.steamid) {
		state.retryDelay = std::chrono::seconds(0);
		state.retryAt = {};
	}
	// CheckPunishments(): a ban disconnects the player - immediately on connect (the original did
	// that inside the query callback) and after ban_delay when the punishments were reloaded.
	// The load was requested for the player the slot held (a load of a slot that was reclaimed
	// never gets here), so the slot's steamid is the ban target.
	if (outcome.ban) {
		KickForBan(slot, outcome.forced, SlotSteamId(slot));
	}
}

void AdminPlugin::KickForBan(int32_t slot, bool delayed, uint64_t steamid) {
	if (slot < 0 || slot >= kMaxSlots || steamid == 0 || __s2sdk_KickClient == nullptr) {
		return; // no s2sdk: the punishment is stored, the disconnect has to happen elsewhere
	}
	// KickClient is g_pEngineServer->DisconnectClient with no validation of its own: it kicks
	// whoever the slot holds at that moment. The slot is reused as soon as the punished player
	// leaves and the ban_delay kick even survives a map change (the punishment load is
	// asynchronous too), so the kick only fires while the slot still carries the punished
	// steamid. ClientSteamID64 needs a connected client but no controller, so a player who is
	// still connecting is matched as well; a ban whose player is not in the slot is enforced
	// again on the next connect.
	const bool immediate = !delayed || m_config.ban_delay <= 0;
	if (immediate) {
		if (IsMapReady() && ClientSteamID64(slot) == steamid) {
			KickClientSlot(slot, static_cast<int32_t>(s2sdk::NetworkDisconnectionReason::Kickbanadded));
		}
		return;
	}
	// The original used a Pisex timer; the plugin runs no s2sdk timers (PITFALLS C9), so the
	// deadline is queued and checked from OnPluginUpdate.
	PendingKick pending;
	pending.slot = slot;
	pending.steamid = steamid;
	pending.due = std::chrono::steady_clock::now() + std::chrono::seconds(m_config.ban_delay);
	m_pendingKicks.push_back(pending);
}

void AdminPlugin::ProcessPendingKicks() {
	if (m_pendingKicks.empty() || !IsMapReady()) {
		return;
	}
	const auto now = std::chrono::steady_clock::now();
	std::vector<PendingKick> remaining;
	remaining.reserve(m_pendingKicks.size());
	for (const PendingKick& kick : m_pendingKicks) {
		if (now < kick.due) {
			remaining.push_back(kick);
			continue;
		}
		// Only kick while the slot still carries the punished steamid; otherwise the ban is
		// enforced again on the next connect.
		if (ClientSteamID64(kick.slot) == kick.steamid) {
			KickClientSlot(kick.slot, static_cast<int32_t>(s2sdk::NetworkDisconnectionReason::Kickbanadded));
		}
	}
	m_pendingKicks = std::move(remaining);
}

void AdminPlugin::ClearPendingKicks() {
	m_pendingKicks.clear();
}

void AdminPlugin::HandlePlayerPunish(int32_t slot, int32_t type, int32_t time, const std::string& reason, int32_t adminRef) {
	for (PlayerPunishCallback callback : m_onPlayerPunish) {
		try {
			if (callback != nullptr) {
				callback(slot, type, time, plg::string(reason), adminRef);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in an OnPlayerPunish listener: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in an OnPlayerPunish listener");
		}
	}
}

void AdminPlugin::HandleOfflinePlayerPunish(const std::string& steamid64, const std::string& name, int32_t type,
											int32_t time, const std::string& reason, int32_t adminRef) {
	for (OfflinePlayerPunishCallback callback : m_onOfflinePlayerPunish) {
		try {
			if (callback != nullptr) {
				callback(plg::string(steamid64), plg::string(name), type, time, plg::string(reason), adminRef);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in an OnOfflinePlayerPunish listener: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in an OnOfflinePlayerPunish listener");
		}
	}
}

void AdminPlugin::HandlePlayerUnpunish(int32_t slot, int32_t type, int32_t adminRef) {
	for (PlayerUnpunishCallback callback : m_onPlayerUnpunish) {
		try {
			if (callback != nullptr) {
				callback(slot, type, adminRef);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in an OnPlayerUnpunish listener: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in an OnPlayerUnpunish listener");
		}
	}
}

void AdminPlugin::HandleOfflinePlayerUnpunish(const std::string& steamid64, int32_t type, int32_t adminRef) {
	for (OfflinePlayerUnpunishCallback callback : m_onOfflinePlayerUnpunish) {
		try {
			if (callback != nullptr) {
				callback(plg::string(steamid64), type, adminRef);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in an OnOfflinePlayerUnpunish listener: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in an OnOfflinePlayerUnpunish listener");
		}
	}
}

void AdminPlugin::HandleCoreError(std::string_view message) {
	LogError(message);
}

// --- notifications (SendPunishmentNotification of the original) ------------------------------

std::string AdminPlugin::FormatTime(int64_t expires) const {
	// FormatTime() of the original: 0 = forever, a past timestamp = expired, otherwise d/h/m/s.
	if (expires == 0) {
		return Phrase("Never");
	}
	int64_t seconds = expires - static_cast<int64_t>(std::time(nullptr));
	if (seconds < 0) {
		return Phrase("Expired");
	}
	std::string parts;
	const int64_t days = seconds / 86400;
	seconds %= 86400;
	const int64_t hours = seconds / 3600;
	seconds %= 3600;
	const int64_t minutes = seconds / 60;
	seconds %= 60;
	if (days > 0) {
		parts += std::to_string(days) + Phrase("Days");
	}
	if (hours > 0) {
		parts += std::to_string(hours) + Phrase("Hours");
	}
	if (minutes > 0) {
		parts += std::to_string(minutes) + Phrase("Minutes");
	}
	if (seconds > 0) {
		parts += std::to_string(seconds) + Phrase("Seconds");
	}
	return parts;
}

std::string AdminPlugin::AdminName(int32_t adminRef) const {
	if (adminRef >= 0 && adminRef < kMaxSlots) {
		// A live punishment carries the slot of the admin (the original read the client name of
		// that slot); the admin record's name wins when it has one.
		if (const AdminRecord* admin = m_core != nullptr ? m_core->FindAdmin(adminRef) : nullptr;
			admin != nullptr && !admin->name.empty()) {
			return admin->name;
		}
		const std::string live = ClientName(adminRef);
		if (!live.empty()) {
			return live;
		}
	}
	return "Console";
}

void AdminPlugin::SendPunishmentNotification(int32_t slot, int32_t type, int32_t time, const std::string& reason,
											 int32_t adminRef) {
	if (m_config.notify_type == 0) {
		return;
	}
	const char* message = nullptr;
	switch (type) {
	case RT_BAN: message = m_config.notify_type == 2 ? "NewBanMessageAll" : "NewBanMessage"; break;
	case RT_MUTE: message = m_config.notify_type == 2 ? "NewMuteMessageAll" : "NewMuteMessage"; break;
	case RT_GAG: message = m_config.notify_type == 2 ? "NewGagMessageAll" : "NewGagMessage"; break;
	case RT_SILENCE: message = m_config.notify_type == 2 ? "NewSilenceMessageAll" : "NewSilenceMessage"; break;
	default: return;
	}
	const std::string admin = AdminName(adminRef);
	const std::string target = ClientName(slot);
	const std::string duration = FormatTime(time == 0 ? 0 : static_cast<int64_t>(std::time(nullptr)) + time);
	if (m_config.notify_type == 1) {
		PrintChat(slot, Phrase(message), CStr(duration), CStr(reason), CStr(admin));
		if (adminRef >= 0 && adminRef < kMaxSlots) {
			const char* adminMessage = nullptr;
			switch (type) {
			case RT_BAN: adminMessage = "NewBanMessageAdmin"; break;
			case RT_MUTE: adminMessage = "NewMuteMessageAdmin"; break;
			case RT_GAG: adminMessage = "NewGagMessageAdmin"; break;
			case RT_SILENCE: adminMessage = "NewSilenceMessageAdmin"; break;
			default: return;
			}
			PrintChat(adminRef, Phrase(adminMessage), CStr(target), CStr(duration), CStr(reason));
		}
		return;
	}
	PrintChatAll(Phrase(message), CStr(admin), CStr(target), CStr(duration), CStr(reason));
}

void AdminPlugin::SendUnpunishmentNotification(int32_t slot, int32_t type, int32_t adminRef) const {
	if (m_config.notify_type != 2) {
		return;
	}
	const char* message = nullptr;
	switch (type) {
	case RT_BAN: message = "UnbanMessageAll"; break;
	case RT_MUTE: message = "UnmuteMessageAll"; break;
	case RT_GAG: message = "UngagMessageAll"; break;
	case RT_SILENCE: message = "UnsilenceMessageAll"; break;
	default: return;
	}
	PrintChatAll(Phrase(message), CStr(AdminName(adminRef)), CStr(ClientName(slot)));
}

// --- punishment flows ------------------------------------------------------------------------

bool AdminPlugin::AddPlayerPunishment(const PlayerIdentity& player, int32_t type, int32_t time, const std::string& reason,
									  int32_t adminRef, bool notify, bool db) {
	if (m_core == nullptr || !m_coreReady) {
		return false;
	}
	if (notify) {
		// PARITY: the original sent the notification before the INSERT.
		SendPunishmentNotification(player.slot, type, time, reason, adminRef);
	}
	const bool applied = m_core->Punish(player, type, time, reason, adminRef);
	if (applied && type == RT_BAN) {
		KickForBan(player.slot, true, player.steamid); // AddPunishment kicked after ban_delay
	}
	(void)db;
	return applied;
}

bool AdminPlugin::AddOfflinePlayerPunishment(const std::string& steamid64, const std::string& name, int32_t type,
											 int32_t time, const std::string& reason, int32_t adminRef) {
	if (m_core == nullptr || !m_coreReady) {
		return false;
	}
	m_core->PunishOffline(steamid64, name, type, time, reason, adminRef);
	return true;
}

bool AdminPlugin::TryAddPunishment(const PlayerIdentity& player, int32_t type, int32_t time, const std::string& reason,
								   int32_t adminRef, bool notify) {
	if (m_core == nullptr || !m_coreReady) {
		return false;
	}
	// TryAddPunishment() of the original: the immunity rules come first, then the notification and
	// the punishment (the API method AddPlayerPunishment skips the immunity check - that is the
	// difference the menus and the commands rely on).
	const AdminRecord* admin = m_core->FindAdmin(adminRef);
	const AdminRecord* target = m_core->FindAdmin(player.slot);
	const int adminImmunity = admin == nullptr ? 0 : admin->immunity;
	const int targetImmunity = target == nullptr ? 0 : target->immunity;
	if (!AdminCore::CanPunish(adminRef, adminImmunity, targetImmunity, m_config.immunity_type)) {
		return false;
	}
	if (notify) {
		SendPunishmentNotification(player.slot, type, time, reason, adminRef);
	}
	const bool applied = m_core->Punish(player, type, time, reason, adminRef);
	if (applied && type == RT_BAN) {
		KickForBan(player.slot, true, player.steamid);
	}
	return applied;
}

void AdminPlugin::TryAddOfflinePunishment(const std::string& steamid64, const std::string& name, int32_t type,
										  int32_t time, const std::string& reason, int32_t adminRef,
										  std::function<void(bool)> onComplete) {
	if (m_core != nullptr && m_coreReady) {
		m_core->TryPunishOffline(steamid64, name, type, time, reason, adminRef, std::move(onComplete));
	} else if (onComplete) {
		onComplete(false);
	}
}

void AdminPlugin::RemovePlayerPunishment(int32_t slot, int32_t type, int32_t adminRef, bool notify) {
	if (m_core == nullptr || !m_coreReady) {
		return;
	}
	if (notify) {
		SendUnpunishmentNotification(slot, type, adminRef);
	}
	m_core->TryUnpunish(slot, type, adminRef);
}

void AdminPlugin::TryRemovePunishment(int32_t slot, int32_t type, int32_t adminRef) {
	if (m_core != nullptr && m_coreReady) {
		m_core->TryUnpunish(slot, type, adminRef);
	}
}

void AdminPlugin::RemoveOfflinePlayerPunishment(const std::string& steamid64, int32_t type, int32_t adminRef) {
	if (m_core != nullptr && m_coreReady) {
		m_core->UnpunishOffline(steamid64, type, adminRef);
	}
}

// --- the admin menu / reload helpers ----------------------------------------------------------

bool AdminPlugin::IsAdminSlot(int32_t slot) const {
	return m_core != nullptr && m_core->IsAdmin(slot);
}

bool AdminPlugin::IsMenuOwnerEligible(const OwnerIdentity& owner) const {
	if (!owner.IsSet() || m_core == nullptr || !m_coreReady) {
		return false;
	}
	// The core's answer is authoritative only for exactly this steamid; a false here means "ask
	// again later", so the queued request is dropped instead of being applied to a stale slot.
	return m_core->IsAdminDataReady(owner.slot, owner.steamId);
}

void AdminPlugin::SetLastCategory(int32_t slot, const std::string& category) {
	if (slot >= 0 && slot < kMaxSlots) {
		m_lastCategory[static_cast<size_t>(slot)] = category;
	}
}

void AdminPlugin::ReloadConfig() {
	// OnReloadConfig of the original: the phrases, the flags and the menu tables are dropped and
	// read again. The database connection stays (it was opened at plugin start).
	LoadCoreConfig();
	LoadPhrases();
	LoadSortingIni();
	if (m_core != nullptr) {
		m_core->SetConfig(m_config);
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		// The rights of the online players are read again with the new flag mapping.
		if (const uint64_t steamid = SlotSteamId(slot); steamid != 0 && m_core != nullptr && m_coreReady) {
			m_core->LoadAdmin(slot, steamid, ClientName(slot));
		}
	}
}

// --- admin maintenance (the AddPlayerAdmin / RemovePlayerAdmin exports) -------------------------

const AdminRecord* AdminPlugin::FindAdmin(int32_t slot) const {
	return m_core == nullptr ? nullptr : m_core->FindAdmin(slot);
}

void AdminPlugin::AddAdmin(std::string name, std::string steamid64, std::string flags, int immunity, int expireSeconds,
						   int64_t groupId, std::string comment, bool db) {
	if (m_core == nullptr || !m_coreReady || steamid64.empty()) {
		return;
	}
	m_core->AddAdmin(std::move(name), std::move(steamid64), std::move(flags), immunity, expireSeconds, groupId,
					 std::move(comment), db);
}

void AdminPlugin::RemoveAdmin(const std::string& steamid64, bool db) {
	if (m_core != nullptr && m_coreReady) {
		m_core->RemoveAdmin(steamid64, db);
	}
}

// --- actions / registry ------------------------------------------------------------------------

void AdminPlugin::SendAction(int32_t slot, const std::string& action, const std::string& param) {
	for (ActionCallback callback : m_onAction) {
		try {
			if (callback != nullptr) {
				callback(slot, plg::string(action), plg::string(param));
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in an OnAction listener: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in an OnAction listener");
		}
	}
}

void AdminPlugin::RegisterCategory(const plg::string& identity, const plg::string& name, CategoryDisplayCallback callback) {
	const std::string key = ToString(identity);
	// PARITY: the OnCategoryRegister listeners fired *before* the category was inserted.
	for (CategoryRegisterCallback listener : m_onCategoryRegister) {
		try {
			if (listener != nullptr) {
				listener(identity);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in an OnCategoryRegister listener: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in an OnCategoryRegister listener");
		}
	}
	MenuCategory& category = m_categories[key];
	category.identity = key;
	category.name = ToString(name);
	category.display = callback;
}

void AdminPlugin::RegisterItem(const plg::string& identity, const plg::string& name, const plg::string& category,
							   const plg::string& flags, ItemDisplayCallback display, ItemSelectCallback select) {
	const std::string categoryKey = ToString(category);
	MenuItem item;
	item.identity = ToString(identity);
	item.name = ToString(name);
	item.category = categoryKey;
	item.display = display;
	item.select = select;
	item.flags = ParsePipeList(ToString(flags)); // the original split the list on '|'
	MenuCategory& entry = m_categories[categoryKey];
	if (entry.identity.empty()) {
		entry.identity = categoryKey;
	}
	entry.items[item.identity] = std::move(item);
}

// --- as_status ----------------------------------------------------------------------------------

void AdminPlugin::PrintStatus(int32_t caller, s2sdk::ConCommandContext context) {
	if (!__s2sdk_ReplyToCommand) {
		return; // unreachable in practice: as_status is only registered with the binding present
	}
	if (m_core == nullptr) {
		s2sdk::ReplyToCommand(context, caller, plg::string("admin_system: the core is not ready"));
		return;
	}
	s2sdk::ReplyToCommand(context, caller, plg::string("admin_system: --- as_status begin ---"));
	s2sdk::ReplyToCommand(context, caller,
						  plg::string("admin_system: prefix='" + m_config.database_prefix + "' admin_server_id=" +
									  std::to_string(m_config.admin_server_id) + " punish_server_id=" +
									  std::to_string(m_config.punish_server_id) + " core_loaded=" +
									  (m_coreLoaded ? "yes" : "no") +
									  " db_pending=" + std::to_string(m_db == nullptr ? 0 : m_db->Pending())));
	int humans = 0;
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!IsMapReady()) {
			break; // no map: not a single client call (PITFALLS B1/B2)
		}
		try {
			if (!HasClient(slot) || IsFakeClientSlot(slot)) {
				continue;
			}
			const uint64_t steamid = SlotSteamId(slot);
			if (steamid == 0) {
				continue;
			}
			++humans;
			const AdminRecord* admin = m_core->FindAdmin(slot);
			std::string line = "admin_system: slot " + std::to_string(slot) + ": steamid " + std::to_string(steamid) +
							   " name='" + ClientName(slot) + "'";
			if (admin != nullptr && admin->IsAdmin()) {
				std::string flags;
				for (const std::string& flag : admin->flags) {
					flags += flag;
				}
				line += " admin=yes id=" + std::to_string(admin->id) + " flags='" + flags + "' immunity=" +
						std::to_string(admin->immunity) + " group=" +
						(admin->group_name.empty() ? std::string("-") : "'" + admin->group_name + "'") +
						" expire=" + (admin->expire_time == 0 ? std::string("never") : FormatTime(admin->expire_time));
			} else {
				line += " admin=no";
			}
			bool anyPunishment = false;
			for (int type = 0; type < kPunishTypeCount; ++type) {
				if (!m_core->IsPunished(slot, type)) {
					continue;
				}
				static constexpr const char* kTypeNames[kPunishTypeCount] = {"ban", "mute", "gag", "silence"};
				line += anyPunishment ? "," : " punishments=";
				anyPunishment = true;
				line += std::string(kTypeNames[type]) + "(" + FormatTime(m_core->PunishmentExpires(slot, type)) + ": " +
						std::string(m_core->PunishmentReason(slot, type)) + ")";
			}
			s2sdk::ReplyToCommand(context, caller, plg::string(line));
		} catch (const std::exception& ex) {
			s2sdk::ReplyToCommand(context, caller,
								  plg::string("admin_system: slot " + std::to_string(slot) + ": error: " + ex.what()));
		} catch (...) {
			s2sdk::ReplyToCommand(context, caller,
								  plg::string("admin_system: slot " + std::to_string(slot) + ": unknown error"));
		}
	}
	s2sdk::ReplyToCommand(context, caller, plg::string("admin_system: connected human players: " + std::to_string(humans)));
	s2sdk::ReplyToCommand(context, caller, plg::string("admin_system: --- as_status end ---"));
}

// --- s2sdk callbacks --------------------------------------------------------------------------

namespace {

void OnClientAuthenticatedListener(int32_t playerSlot, uint64_t steamid) {
	try {
		g_adminPlugin.HandleClientAuthenticated(playerSlot, steamid);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in OnClientAuthenticated: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in OnClientAuthenticated");
	}
}

void OnClientDisconnectListener(int32_t playerSlot, s2sdk::NetworkDisconnectionReason reason) {
	(void)reason;
	try {
		g_adminPlugin.HandleClientDisconnect(playerSlot);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in OnClientDisconnect: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in OnClientDisconnect");
	}
}

// static_name = 1: the name of an admin is forced to the database name. "jointeam" reaches the
// global listener (PITFALLS C11: the game parses it in its ClientCommand path, so it is not a
// ConCommand), and answering Continue lets the engine's own handling run as before.
s2sdk::ResultType OnStaticNameCommandListener(int32_t caller, s2sdk::ConCommandContext /*context*/,
											  const plg::vector<plg::string>& args) {
	try {
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const std::string_view name(args[0].data(), args[0].size());
		if (name != "jointeam") {
			return s2sdk::ResultType::Continue; // the original hooked only "jointeam"
		}
		if (IsSlotInRange(caller) && HasController(caller)) {
			if (const AdminRecord* admin = g_adminPlugin.FindAdmin(caller);
				admin != nullptr && admin->id > 0 && !admin->name.empty()) {
				SetClientNameSafe(caller, admin->name);
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the static-name jointeam hook: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the static-name jointeam hook");
	}
	return s2sdk::ResultType::Continue; // the original let the command through (MRES_IGNORED)
}

s2sdk::ResultType OnRoundStartEvent(const plg::string& /*name*/, void* /*event*/, bool /*dontBroadcast*/) {
	try {
		for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
			if (!HasClient(slot)) {
				continue;
			}
			if (const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
				admin != nullptr && admin->id > 0 && !admin->name.empty()) {
				SetClientNameSafe(slot, admin->name);
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the static-name round_start hook: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the static-name round_start hook");
	}
	return s2sdk::ResultType::Continue;
}

// The chat listeners of the original: s2sdk hands every say/say_team line here as
// (say, first-token, rest...). The message is rebuilt without quoting, exactly like pisexcompat's
// chat trampoline, so the gag/silence check and the own-reason capture see the text the player
// typed and the first character test ('!'/'/') keeps working.
s2sdk::ResultType OnSayListener(int32_t caller, s2sdk::ConCommandContext /*context*/,
								const plg::vector<plg::string>& args) {
	try {
		if (args.size() < 2) {
			return s2sdk::ResultType::Continue;
		}
		const bool team = std::string_view(args[0].data(), args[0].size()) == "say_team";
		std::string content;
		for (std::size_t i = 1; i < args.size(); ++i) {
			if (i != 1) {
				content.push_back(' ');
			}
			content.append(args[i].data(), args[i].size());
		}
		const bool allow = g_adminPlugin.OnChatPre(caller, content.c_str(), team);
		return allow ? s2sdk::ResultType::Continue : s2sdk::ResultType::Handled;
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the chat listener: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the chat listener");
	}
	return s2sdk::ResultType::Continue;
}

void OnMapGateStart() {
	try {
		g_adminPlugin.OnMapStartHook();
	} catch (...) {
	}
}

void OnMapGateEnd() {
	try {
		g_adminPlugin.OnMapEndHook();
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the map-end hook: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the map-end hook");
	}
}

bool MenuOwnerEligibleTrampoline(void* user, const OwnerIdentity& owner) {
	const AdminPlugin* plugin = static_cast<const AdminPlugin*>(user);
	if (plugin == nullptr) {
		return false;
	}
	try {
		return plugin->IsMenuOwnerEligible(owner);
	} catch (...) {
		return false;
	}
}

} // namespace

} // namespace admin_system

// --- the exported methods (IAdminApi of the original, 60 exports) -------------------------------
// Every one of them catches its exceptions: they are called across the plugify boundary from other
// plugins, where a throw would unwind into the module (and from there into the engine).

using namespace admin_system;

namespace {

plg::vector<plg::string> ToPlgList(const std::vector<std::string>& values) {
	plg::vector<plg::string> result;
	result.reserve(values.size());
	for (const std::string& value : values) {
		result.emplace_back(value);
	}
	return result;
}

} // namespace

extern "C" PLUGIN_API float GetPluginVersion() {
	try {
		return 1.2f; // 1.2.0 (the original returned atof(plugin version))
	} catch (...) {
		return 0.0f;
	}
}

extern "C" PLUGIN_API plg::string GetTranslation(const plg::string& key) {
	try {
		return plg::string(g_adminPlugin.Phrase(ToString(key).c_str()));
	} catch (...) {
		return plg::string(key);
	}
}

extern "C" PLUGIN_API plg::vector<plg::string> GetDatabaseConfig() {
	try {
		const plgdb::Config& config = g_adminPlugin.DatabaseConfig();
		plg::vector<plg::string> result;
		result.reserve(7);
		result.emplace_back(config.driver == plgdb::Driver::SQLite ? "sqlite" : "mysql");
		// SQLite hands out the file path in the host slot; there is no user/password then.
		result.emplace_back(config.driver == plgdb::Driver::SQLite ? config.sqlitePath : config.host);
		result.emplace_back(std::to_string(config.port));
		result.emplace_back(config.driver == plgdb::Driver::SQLite ? "" : config.user);
		result.emplace_back(config.driver == plgdb::Driver::SQLite ? "" : config.password);
		result.emplace_back(config.database);
		result.emplace_back(g_adminPlugin.Cfg().database_prefix);
		return result;
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API int32_t GetMessageType() {
	try {
		return g_adminPlugin.Cfg().message_type;
	} catch (...) {
		return 0;
	}
}

extern "C" PLUGIN_API bool IsCoreLoaded() {
	try {
		return g_adminPlugin.IsCoreLoaded();
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API void RegisterCategory(const plg::string& identity, const plg::string& name,
											CategoryDisplayCallback callback) {
	try {
		g_adminPlugin.RegisterCategory(identity, name, callback);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: RegisterCategory failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: RegisterCategory failed");
	}
}

extern "C" PLUGIN_API void RegisterItem(const plg::string& identity, const plg::string& name, const plg::string& category,
										const plg::string& flags, ItemDisplayCallback display, ItemSelectCallback select) {
	try {
		g_adminPlugin.RegisterItem(identity, name, category, flags, display, select);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: RegisterItem failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: RegisterItem failed");
	}
}

extern "C" PLUGIN_API bool HasPermission(int32_t slot, const plg::string& permission) {
	try {
		// The core takes a string_view: pass a view over the caller's plg::string instead of
		// building a std::string per call (this export is called by other plugins, possibly in
		// their per-tick paths).
		return g_adminPlugin.CoreOrNull() != nullptr &&
			   g_adminPlugin.CoreOrNull()->HasPermission(slot, std::string_view(permission.data(), permission.size()));
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API bool HasFlag(int32_t slot, const plg::string& flag) {
	try {
		return g_adminPlugin.CoreOrNull() != nullptr &&
			   g_adminPlugin.CoreOrNull()->HasFlag(slot, std::string_view(flag.data(), flag.size()));
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API bool IsAdmin(int32_t slot) {
	try {
		return g_adminPlugin.CoreOrNull() != nullptr && g_adminPlugin.CoreOrNull()->IsAdmin(slot);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API bool IsClientAdminDataReady(int32_t slot, uint64_t steamid) {
	try {
		// False whenever the answer would not be authoritative: no core yet (database/schema not
		// up, or the plugin is unloading), a slot the core does not track, a load that has not
		// answered (Pending/Failed) and a steamid the current answer was not computed for (the
		// slot was reclaimed by another player, or the caller does not know the identity yet).
		// A consumer must not turn this false into "not an admin": it only says "ask again later".
		// After it answers true, IsAdmin()/HasPermission()/HasFlag() are authoritative for this
		// steamid - and they may still answer false (the player really is not an admin).
		const AdminCore* core = g_adminPlugin.CoreOrNull();
		return core != nullptr && core->IsAdminDataReady(slot, steamid);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API plg::string GetFlagName(const plg::string& flag) {
	try {
		return plg::string(g_adminPlugin.Cfg().FlagName(ToString(flag)));
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API plg::vector<plg::string> GetPermissionsByFlag(const plg::string& flag) {
	try {
		return ToPlgList(g_adminPlugin.Cfg().PermissionsByFlag(ToString(flag)));
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API plg::vector<plg::string> GetAdminFlags(int32_t slot) {
	try {
		const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
		return ToPlgList(admin == nullptr ? std::vector<std::string>{} : admin->flags);
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API plg::vector<plg::string> GetAdminPermissions(int32_t slot) {
	try {
		const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
		return ToPlgList(admin == nullptr ? std::vector<std::string>{} : admin->permissions);
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API int32_t GetAdminImmunity(int32_t slot) {
	try {
		const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
		return admin == nullptr ? 0 : admin->immunity;
	} catch (...) {
		return 0;
	}
}

extern "C" PLUGIN_API int64_t GetAdminExpireTime(int32_t slot) {
	try {
		const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
		return admin == nullptr ? -1 : admin->expire_time;
	} catch (...) {
		return -1;
	}
}

extern "C" PLUGIN_API plg::string GetAdminName(int32_t slot) {
	try {
		if (slot == -1) {
			return plg::string("Console");
		}
		if (const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
			admin != nullptr && admin->id > 0 && !admin->name.empty()) {
			return plg::string(admin->name);
		}
		return plg::string(ClientName(slot));
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API int32_t GetAdminGroupID(int32_t slot) {
	try {
		const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
		return admin == nullptr || admin->id <= 0 ? -1 : static_cast<int32_t>(admin->group_id);
	} catch (...) {
		return -1;
	}
}

extern "C" PLUGIN_API plg::string GetAdminGroupName(int32_t slot) {
	try {
		const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
		return admin == nullptr || admin->id <= 0 ? plg::string() : plg::string(admin->group_name);
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API int32_t GetImmunityType() {
	try {
		return g_adminPlugin.Cfg().immunity_type;
	} catch (...) {
		return 0;
	}
}

extern "C" PLUGIN_API void AddPlayerAdmin(const plg::string& name, const plg::string& steamid64, const plg::string& flags,
										  int32_t immunity, int32_t expireTime, int32_t groupID, const plg::string& comment,
										  bool db) {
	try {
		g_adminPlugin.AddAdmin(ToString(name), ToString(steamid64), ToString(flags), immunity, expireTime, groupID,
							   ToString(comment), db);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: AddPlayerAdmin failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: AddPlayerAdmin failed");
	}
}

extern "C" PLUGIN_API void RemovePlayerAdmin(const plg::string& steamid64, bool db) {
	try {
		g_adminPlugin.RemoveAdmin(ToString(steamid64), db);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: RemovePlayerAdmin failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: RemovePlayerAdmin failed");
	}
}

extern "C" PLUGIN_API void AddPlayerLocalFlag(int32_t slot, const plg::string& flag) {
	try {
		if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
			core->AddLocalFlag(slot, ToString(flag));
		}
	} catch (...) {
	}
}

extern "C" PLUGIN_API void RemovePlayerLocalFlag(int32_t slot, const plg::string& flag) {
	try {
		if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
			core->RemoveLocalFlag(slot, ToString(flag));
		}
	} catch (...) {
	}
}

extern "C" PLUGIN_API void AddPlayerLocalPermission(int32_t slot, const plg::string& permission) {
	try {
		if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
			core->AddLocalPermission(slot, ToString(permission));
		}
	} catch (...) {
	}
}

extern "C" PLUGIN_API void RemovePlayerLocalPermission(int32_t slot, const plg::string& permission) {
	try {
		if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
			core->RemoveLocalPermission(slot, ToString(permission));
		}
	} catch (...) {
	}
}

extern "C" PLUGIN_API void AddPlayerLocalImmunity(int32_t slot, int32_t immunity) {
	try {
		if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
			core->AddLocalImmunity(slot, immunity);
		}
	} catch (...) {
	}
}

extern "C" PLUGIN_API void RemovePlayerLocalImmunity(int32_t slot) {
	try {
		if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
			core->RemoveLocalImmunity(slot);
		}
	} catch (...) {
	}
}

extern "C" PLUGIN_API bool IsPlayerPunished(int32_t slot, int32_t type) {
	try {
		return g_adminPlugin.CoreOrNull() != nullptr && g_adminPlugin.CoreOrNull()->IsPunished(slot, type);
	} catch (...) {
		return false;
	}
}

extern "C" PLUGIN_API int64_t GetPlayerPunishmentExpired(int32_t slot, int32_t type) {
	try {
		return g_adminPlugin.CoreOrNull() == nullptr ? -1 : g_adminPlugin.CoreOrNull()->PunishmentExpires(slot, type);
	} catch (...) {
		return -1;
	}
}

extern "C" PLUGIN_API plg::string GetPlayerPunishmentReason(int32_t slot, int32_t type) {
	try {
		return plg::string(g_adminPlugin.CoreOrNull() == nullptr
							   ? std::string_view{}
							   : g_adminPlugin.CoreOrNull()->PunishmentReason(slot, type));
	} catch (...) {
		return {};
	}
}

extern "C" PLUGIN_API void AddPlayerPunishment(int32_t slot, int32_t type, int32_t time, const plg::string& reason,
											   int32_t adminID, bool notify, bool db) {
	try {
		g_adminPlugin.AddPlayerPunishment(g_adminPlugin.IdentityOf(slot), type, time, ToString(reason), adminID, notify, db);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: AddPlayerPunishment failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: AddPlayerPunishment failed");
	}
}

extern "C" PLUGIN_API void AddOfflinePlayerPunishment(const plg::string& steamid64, const plg::string& name, int32_t type,
													  int32_t time, const plg::string& reason, int32_t adminID) {
	try {
		g_adminPlugin.AddOfflinePlayerPunishment(ToString(steamid64), ToString(name), type, time, ToString(reason), adminID);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: AddOfflinePlayerPunishment failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: AddOfflinePlayerPunishment failed");
	}
}

extern "C" PLUGIN_API void RemovePlayerPunishment(int32_t slot, int32_t type, int32_t adminID, bool notify) {
	try {
		g_adminPlugin.RemovePlayerPunishment(slot, type, adminID, notify);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: RemovePlayerPunishment failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: RemovePlayerPunishment failed");
	}
}

extern "C" PLUGIN_API void RemoveOfflinePlayerPunishment(const plg::string& steamid64, int32_t type, int32_t adminID) {
	try {
		g_adminPlugin.RemoveOfflinePlayerPunishment(ToString(steamid64), type, adminID);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: RemoveOfflinePlayerPunishment failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: RemoveOfflinePlayerPunishment failed");
	}
}

extern "C" PLUGIN_API void ShowAdminMenu(int32_t slot) {
	try {
		g_adminPlugin.ShowAdminMenu(slot);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: ShowAdminMenu failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: ShowAdminMenu failed");
	}
}

extern "C" PLUGIN_API void ShowAdminCategoryMenu(int32_t slot, const plg::string& category) {
	try {
		g_adminPlugin.ShowAdminCategoryMenu(slot, ToString(category));
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: ShowAdminCategoryMenu failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: ShowAdminCategoryMenu failed");
	}
}

extern "C" PLUGIN_API void ShowAdminLastCategoryMenu(int32_t slot) {
	try {
		g_adminPlugin.ShowAdminLastCategoryMenu(slot);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: ShowAdminLastCategoryMenu failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: ShowAdminLastCategoryMenu failed");
	}
}

extern "C" PLUGIN_API void ShowAdminItemMenu(int32_t slot, const plg::string& category, const plg::string& identity) {
	try {
		g_adminPlugin.ShowAdminItemMenu(slot, ToString(category), ToString(identity));
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: ShowAdminItemMenu failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: ShowAdminItemMenu failed");
	}
}

extern "C" PLUGIN_API void SendAction(int32_t slot, const plg::string& action, const plg::string& param) {
	try {
		g_adminPlugin.SendAction(slot, ToString(action), ToString(param));
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: SendAction failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: SendAction failed");
	}
}

// The ten Register/Unregister pairs (the SourceMM::PluginId parameter of the original).
extern "C" PLUGIN_API void OnAdminConnected_Register(AdminConnectedCallback callback) {
	try {
		AddListener(g_adminPlugin.AdminConnectedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnAdminConnected_Unregister(AdminConnectedCallback callback) {
	try {
		RemoveListener(g_adminPlugin.AdminConnectedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnCoreLoaded_Register(CoreLoadedCallback callback) {
	try {
		AddListener(g_adminPlugin.CoreLoadedListeners(), callback);
		// A plugin that registers after the core became ready never sees the event otherwise
		// (the original had the same IsCoreLoaded() escape hatch, see API.md).
		if (g_adminPlugin.IsCoreLoaded() && callback != nullptr) {
			callback();
		}
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnCoreLoaded_Unregister(CoreLoadedCallback callback) {
	try {
		RemoveListener(g_adminPlugin.CoreLoadedListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnCategoryRegister_Register(CategoryRegisterCallback callback) {
	try {
		AddListener(g_adminPlugin.CategoryRegisterListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnCategoryRegister_Unregister(CategoryRegisterCallback callback) {
	try {
		RemoveListener(g_adminPlugin.CategoryRegisterListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnAction_Register(ActionCallback callback) {
	try {
		AddListener(g_adminPlugin.ActionListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnAction_Unregister(ActionCallback callback) {
	try {
		RemoveListener(g_adminPlugin.ActionListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerPunish_Register(PlayerPunishCallback callback) {
	try {
		AddListener(g_adminPlugin.PlayerPunishListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerPunish_Unregister(PlayerPunishCallback callback) {
	try {
		RemoveListener(g_adminPlugin.PlayerPunishListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnOfflinePlayerPunish_Register(OfflinePlayerPunishCallback callback) {
	try {
		AddListener(g_adminPlugin.OfflinePlayerPunishListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnOfflinePlayerPunish_Unregister(OfflinePlayerPunishCallback callback) {
	try {
		RemoveListener(g_adminPlugin.OfflinePlayerPunishListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerUnpunish_Register(PlayerUnpunishCallback callback) {
	try {
		AddListener(g_adminPlugin.PlayerUnpunishListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerUnpunish_Unregister(PlayerUnpunishCallback callback) {
	try {
		RemoveListener(g_adminPlugin.PlayerUnpunishListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnOfflinePlayerUnpunish_Register(OfflinePlayerUnpunishCallback callback) {
	try {
		AddListener(g_adminPlugin.OfflinePlayerUnpunishListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnOfflinePlayerUnpunish_Unregister(OfflinePlayerUnpunishCallback callback) {
	try {
		RemoveListener(g_adminPlugin.OfflinePlayerUnpunishListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerPunishPre_Register(PlayerPunishPreCallback callback) {
	try {
		AddListener(g_adminPlugin.PlayerPunishPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnPlayerPunishPre_Unregister(PlayerPunishPreCallback callback) {
	try {
		RemoveListener(g_adminPlugin.PlayerPunishPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnOfflinePlayerPunishPre_Register(OfflinePlayerPunishPreCallback callback) {
	try {
		AddListener(g_adminPlugin.OfflinePlayerPunishPreListeners(), callback);
	} catch (...) {
	}
}

extern "C" PLUGIN_API void OnOfflinePlayerPunishPre_Unregister(OfflinePlayerPunishPreCallback callback) {
	try {
		RemoveListener(g_adminPlugin.OfflinePlayerPunishPreListeners(), callback);
	} catch (...) {
	}
}

PLUGIFY_PLUGIN(PLUGIN_API, &admin_system::g_adminPlugin)
