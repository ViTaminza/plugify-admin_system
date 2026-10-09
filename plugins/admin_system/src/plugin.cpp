// admin_system — engine side: lifecycle, configuration, s2sdk listeners, the once-per-second
// reconcile/expiry timer, the admin_core hook bodies, the punishment notifications of the
// original and the 60 exported methods (the IAdminApi of the Pisex cs2-admin_system plugin).
//
// Ported from ref/pisex/cs2-admin_system (admin_system.cpp, database.cpp, menus.cpp) onto
// libs/pisexcompat + admin_core. Commands live in commands.cpp, the menu in menus.cpp.
//
// Rules this file follows:
//   * nothing here blocks on the database except the startup path (CreateSchema in EnsureCore);
//   * every s2sdk/plugify callback catches its exceptions (a throw into the module would kill the
//     server, not the plugin);
//   * no client query before a map exists - pisexcompat's player wrappers return neutral values
//     until then, and the reconcile only runs from OnPluginUpdate (pumped with a live map);
//   * the plugin never returns Failed from OnPluginStart (that stops every plugify plugin); a
//     missing binding or a broken config leaves it running as a no-op core.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <pisex/pisex.hpp>

#include <admin_system/admin_system.hpp>

#include "admin_plugin.hpp"

namespace admin_system {

AdminPlugin g_adminPlugin;

// --- logging / small helpers -----------------------------------------------------------

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
constexpr float kSecondTimerInterval = 1.0f;

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

} // namespace

// --- configuration ---------------------------------------------------------------------

bool AdminPlugin::LoadCoreConfig() {
	// LoadConfig() of the original, key for key. The built-in defaults are the values of the
	// shipped core.ini, so a reload that drops a key cannot silently change the behaviour.
	Config config;
	config.database_prefix = std::string(kDefaultPrefix);

	pisex::KeyValues kv("Config");
	if (!kv.LoadFromFile("addons/configs/admin_system/core.ini")) {
		LogError("admin_system: cannot read core.ini - keeping the built-in defaults");
		m_config = std::move(config);
		return false;
	}

	if (const std::string_view prefix = kv.GetStringValue("database_prefix", kDefaultPrefix); !prefix.empty()) {
		config.database_prefix = std::string(prefix);
	}
	config.admin_server_id = kv.GetInt("admin_server_id", -1);
	config.punish_server_id = kv.GetInt("punish_server_id", -1);
	config.punish_ip = kv.GetBool("punish_ip", false);
	config.time_reason_type = kv.GetInt("time_reason_type", 0);
	config.ban_delay = kv.GetInt("ban_delay", 0);
	config.notify_type = kv.GetInt("notify_type", 0);
	config.unpunish_type = kv.GetInt("unpunish_type", 0);
	config.immunity_type = kv.GetInt("immunity_type", 0);
	config.punish_offline_count = kv.GetInt("punish_offline_count", 0);
	config.unpunish_offline_count = kv.GetInt("unpunish_offline_count", 0);
	config.static_name = kv.GetBool("static_name", false);
	config.message_type = kv.GetInt("message_type", 0);
	config.own_reason = kv.GetBool("own_reason", false);
	config.mute_ignore_commands = kv.GetBool("mute_ignore_commands", true);

	if (pisex::KeyValues* defaults = kv.FindKey("default_permissions", false)) {
		FOR_EACH_VALUE(defaults, value) {
			if (value->GetInt(nullptr, 0) != 0) {
				config.default_permissions.emplace_back(value->GetName());
			}
		}
	}

	if (pisex::KeyValues* flags = kv.FindKey("flags", false)) {
		FOR_EACH_SUBKEY(flags, flag) {
			Flag parsed;
			parsed.name = flag->GetString("name", "");
			if (pisex::KeyValues* access = flag->FindKey("access", false)) {
				FOR_EACH_VALUE(access, permission) {
					if (permission->GetInt(nullptr, 0) != 0) {
						parsed.permissions.emplace_back(permission->GetName());
					}
				}
			}
			config.flags[flag->GetName()] = std::move(parsed);
		}
	}

	// The punishment menu tables: either "reasons" + "times" (time_reason_type = 0) or
	// "times_reasons" (time_reason_type = 1), exactly as LoadConfig() reads them.
	const char* typeKeys[kPunishmentTypes] = {"bans", "mutes", "gags", "silences"};
	if (config.time_reason_type != 0) {
		if (pisex::KeyValues* timesReasons = kv.FindKey("times_reasons", false)) {
			for (int type = 0; type < kPunishmentTypes; ++type) {
				pisex::KeyValues* perType = timesReasons->FindKey(typeKeys[type], false);
				if (perType == nullptr) {
					continue;
				}
				FOR_EACH_SUBKEY(perType, reason) {
					ReasonTimes entry;
					entry.reason = reason->GetName();
					FOR_EACH_VALUE(reason, time) {
						entry.times.emplace_back(std::atoi(time->GetName()), time->GetString(nullptr));
					}
					config.reasons_times[type].push_back(std::move(entry));
				}
			}
		}
	} else {
		if (pisex::KeyValues* reasons = kv.FindKey("reasons", false)) {
			for (int type = 0; type < kPunishmentTypes; ++type) {
				pisex::KeyValues* perType = reasons->FindKey(typeKeys[type], false);
				if (perType == nullptr) {
					continue;
				}
				FOR_EACH_VALUE(perType, reason) {
					if (reason->GetInt(nullptr, 0) != 0) {
						config.reasons[type].emplace_back(reason->GetName());
					}
				}
			}
		}
		if (pisex::KeyValues* times = kv.FindKey("times", false)) {
			for (int type = 0; type < kPunishmentTypes; ++type) {
				pisex::KeyValues* perType = times->FindKey(typeKeys[type], false);
				if (perType == nullptr) {
					continue;
				}
				FOR_EACH_VALUE(perType, time) {
					config.times[type].emplace_back(std::atoi(time->GetName()), time->GetString(nullptr));
				}
			}
		}
	}

	m_config = std::move(config);
	return true;
}

bool AdminPlugin::LoadSortingIni() {
	m_sortCategories.clear();
	m_sortItems.clear();

	pisex::KeyValues kv("Config");
	if (!kv.LoadFromFile("addons/configs/admin_system/sorting.ini")) {
		LogError("admin_system: cannot read sorting.ini - the menus fall back to the registry order");
		return false;
	}
	FOR_EACH_SUBKEY(&kv, category) {
		const std::string name = category->GetName();
		m_sortCategories.push_back(name);
		FOR_EACH_VALUE(category, item) {
			m_sortItems[name].emplace_back(item->GetString(nullptr));
		}
	}
	return true;
}

bool AdminPlugin::LoadPhrasesFiles() {
	// The server's admin_system.phrases.txt holds the phrases of the core *and* of the modules
	// (the modules ask the core for their keys), exactly like on the Metamod server.
	pisex::g_translations.clear();
	if (!pisex::LoadPhrases("admin_system.phrases.txt")) {
		LogError("admin_system: cannot read admin_system.phrases.txt - phrase keys are printed as-is");
		return false;
	}
	return true;
}

// --- startup -------------------------------------------------------------------------

bool AdminPlugin::EnsureCore() {
	// Database settings: the databases.cfg entry of the original (section "admin_system"), with
	// "driver" and "sqlite_path" as optional keys for a SQLite server.
	plgdb::Config database;
	database.driver = plgdb::Driver::MySQL;
	pisex::KeyValues kv("Databases");
	bool haveConfig = false;
	if (kv.LoadFromFile("addons/configs/databases.cfg")) {
		if (pisex::KeyValues* entry = kv.FindKey("admin_system", false)) {
			haveConfig = true;
			if (const std::string driver = entry->GetString("driver", "mysql"); driver == "sqlite") {
				database.driver = plgdb::Driver::SQLite;
			}
			database.host = entry->GetString("host", "127.0.0.1");
			database.port = static_cast<uint16_t>(std::max(1, std::min(65535, entry->GetInt("port", 3306))));
			database.user = entry->GetString("user", "");
			database.password = entry->GetString("pass", "");
			database.database = entry->GetString("database", "admin_system");
			if (database.driver == plgdb::Driver::SQLite) {
				database.sqlitePath = pisex::ResolveDataPath(entry->GetString("sqlite_path", "admin_system.sqlite3"));
			}
		}
	}
	if (!haveConfig) {
		// Fail soft: a local SQLite file keeps the plugin usable without a databases.cfg.
		LogError("admin_system: no 'admin_system' entry in addons/configs/databases.cfg - "
				 "falling back to a local SQLite database");
		database.driver = plgdb::Driver::SQLite;
		database.sqlitePath = pisex::ResolveDataPath("admin_system.sqlite3");
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
	return true;
}

void AdminPlugin::RegisterStaticNameHooks() {
	// static_name = 1: the name of an admin is forced to the database name (the original hooked the
	// jointeam console command and the round_start event for this).
	if (!m_config.static_name) {
		return;
	}
	pisex::g_pUtils->RegCommand({"jointeam"}, {}, [](int32_t slot, const char*) {
		try {
			if (slot >= 0 && slot < kMaxSlots) {
				const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
				if (admin != nullptr && admin->id > 0 && !admin->name.empty()) {
					pisex::g_pPlayers->SetPlayerName(slot, admin->name.c_str());
				}
			}
		} catch (...) {
			LogError("admin_system: unknown exception in the static-name jointeam hook");
		}
		return false; // the original let the command through (MRES_IGNORED)
	});
	pisex::g_pUtils->HookEvent("round_start", [](const char*, pisex::IGameEvent*, bool) {
		try {
			for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
				if (!pisex::g_pPlayers->IsConnected(slot)) {
					continue;
				}
				const AdminRecord* admin = g_adminPlugin.FindAdmin(slot);
				if (admin != nullptr && admin->id > 0 && !admin->name.empty()) {
					pisex::g_pPlayers->SetPlayerName(slot, admin->name.c_str());
				}
			}
		} catch (...) {
			LogError("admin_system: unknown exception in the static-name round_start hook");
		}
	});
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
		if (!pisex::Init("admin_system")) {
			LogError("admin_system: the s2sdk bindings are not available (is the s2sdk plugin >= 2.18.2 loaded?) - "
					 "the engine half of the plugin stays off, the API keeps serving other plugins");
		}
		LoadCoreConfig();
		LoadPhrasesFiles();
		LoadSortingIni();

		if (!EnsureCore()) {
			// Fail soft: the plugin stays loaded, the API answers with neutral values and the
			// commands report that the core is not ready.
			LogError("admin_system: the core is not ready - the plugin runs as a no-op");
			return plg::PluginResult{};
		}

		// The s2sdk listeners of the original (authentication + disconnect) and the chat listener
		// for gags/silences. Every direct s2sdk call is guarded: a missing binding must degrade to
		// a no-op, never to a call through a null function pointer.
		if (__s2sdk_OnClientAuthenticated_Register && __s2sdk_OnClientAuthenticated_Unregister &&
			__s2sdk_OnClientDisconnect_Post_Register && __s2sdk_OnClientDisconnect_Post_Unregister) {
			s2sdk::OnClientAuthenticated_Register(&OnClientAuthenticatedListener);
			s2sdk::OnClientDisconnect_Post_Register(&OnClientDisconnectListener);
			m_listenersRegistered = true;
		} else {
			LogError("admin_system: the s2sdk client listeners are not available - admins and punishments are only "
					 "loaded by the reconcile loop");
		}
		pisex::AddChatListenerPre([](int slot, const char* content, bool team) {
			return g_adminPlugin.OnChatPre(slot, content, team);
		});

		RegisterCommands();  // commands.cpp
		RegisterCoreMenus();
		RegisterStaticNameHooks();

		// Everything that needs a map (and the deferred startup work of Pisex):
		pisex::g_pUtils->StartupServer([] {
			try {
				g_adminPlugin.Core().RemoveExpiredAdmins();
				g_adminPlugin.BroadcastCoreLoaded();
				g_adminPlugin.ReconcileSlots();
			} catch (const std::exception& ex) {
				LogError(std::string("admin_system: exception in the startup callback: ") + ex.what());
			} catch (...) {
				LogError("admin_system: unknown exception in the startup callback");
			}
		});

		// The one-second timer of the original (punishment/admin expiry) plus the reconcile.
		m_secondTimer = pisex::g_pUtils->CreateTimer(kSecondTimerInterval, [] {
			try {
				g_adminPlugin.OnSecondTick();
			} catch (const std::exception& ex) {
				LogError(std::string("admin_system: exception in the second timer: ") + ex.what());
			} catch (...) {
				LogError("admin_system: unknown exception in the second timer");
			}
			return kSecondTimerInterval;
		});

		LogInfo("admin_system: plugin started (prefix '" + m_config.database_prefix + "', servers " +
				std::to_string(m_config.admin_server_id) + "/" + std::to_string(m_config.punish_server_id) + ")");
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in OnPluginStart: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in OnPluginStart");
	}
	(void)m_secondTimer;
	return plg::PluginResult{};
}

plg::PluginResult AdminPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	try {
		// pisexcompat first: it applies the deferred startup callbacks, the timer reschedules and
		// the menu requests that came out of engine callbacks (which run under s2sdk's locks).
		pisex::Poll();
		if (m_db != nullptr) {
			m_db->Poll();
		}
		// Fallback for a server whose s2sdk timer binding is missing: keep the one-second work
		// (expiry, reconcile) running from the update pump.
		if (m_secondTimer == nullptr) {
			m_updateAccumulator += dt;
			if (m_updateAccumulator >= std::chrono::seconds(1)) {
				m_updateAccumulator = std::chrono::milliseconds(0);
				OnSecondTick();
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in OnPluginUpdate: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in OnPluginUpdate");
	}
	return plg::PluginResult{};
}

plg::PluginResult AdminPlugin::OnPluginEnd() {
	try {
		if (m_listenersRegistered) {
			s2sdk::OnClientAuthenticated_Unregister(&OnClientAuthenticatedListener);
			s2sdk::OnClientDisconnect_Post_Unregister(&OnClientDisconnectListener);
			m_listenersRegistered = false;
		}
		UnregisterCommands(); // as_status is an s2sdk command, pisex::Shutdown() does not know it
		pisex::Shutdown();
		// The core has to go before the database (its callbacks hold a lifetime token).
		m_core.reset();
		m_db.reset();
		m_coreLoaded = false;
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in OnPluginEnd: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in OnPluginEnd");
	}
	return plg::PluginResult{};
}

// --- engine side -------------------------------------------------------------------------

void AdminPlugin::HandleClientAuthenticated(int32_t playerSlot, uint64_t steamid) {
	if (m_core == nullptr || playerSlot < 0 || playerSlot >= kMaxSlots) {
		return;
	}
	SlotState& state = m_slots[static_cast<size_t>(playerSlot)];
	if (state.requested && state.steamid == steamid) {
		return; // already requested (the reconcile may have been faster than the event)
	}
	state.steamid = steamid;
	state.requested = true;
	state.name = pisex::g_pPlayers->GetPlayerName(playerSlot);
	state.requestedAt = std::chrono::steady_clock::now();

	// CheckPunishments() + CheckPermissions() of the original's OnClientAuthorized/ClientConnect.
	m_core->ResetSlot(playerSlot);
	m_core->LoadPunishments(playerSlot, steamid, pisex::g_pPlayers->GetIpAddress(playerSlot));
	m_core->LoadAdmin(playerSlot, steamid, state.name);
}

void AdminPlugin::HandleClientDisconnect(int32_t playerSlot) {
	if (playerSlot < 0 || playerSlot >= kMaxSlots) {
		return;
	}
	// The original kept the last players around for the offline menus.
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
	m_slots[static_cast<size_t>(playerSlot)] = SlotState{};
	m_ownReasons[static_cast<size_t>(playerSlot)] = OwnReasonState{};
	m_lastCategory[static_cast<size_t>(playerSlot)].clear();
	if (m_core != nullptr) {
		m_core->ResetSlot(playerSlot);
	}
}

bool AdminPlugin::IsSlotHuman(int32_t slot) const {
	if (slot < 0 || slot >= kMaxSlots || !pisex::IsGameReady()) {
		return false;
	}
	return pisex::g_pPlayers->IsConnected(slot) && pisex::g_pPlayers->IsAuthenticated(slot) &&
		   !pisex::g_pPlayers->IsFakeClient(slot);
}

uint64_t AdminPlugin::SlotSteamId(int32_t slot) const {
	if (slot < 0 || slot >= kMaxSlots) {
		return 0;
	}
	if (IsSlotHuman(slot)) {
		if (const uint64_t live = pisex::g_pPlayers->GetSteamID64(slot); live != 0) {
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
		player.name = pisex::g_pPlayers->GetPlayerName(slot);
		if (player.name.empty()) {
			player.name = m_slots[static_cast<size_t>(slot)].name;
		}
		player.ip = pisex::g_pPlayers->GetIpAddress(slot);
	}
	return player;
}

void AdminPlugin::ReconcileSlots() {
	if (m_core == nullptr || !pisex::IsGameReady()) {
		return;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (!pisex::g_pPlayers->IsConnected(slot)) {
				if (m_slots[static_cast<size_t>(slot)].steamid != 0) {
					// No disconnect event (or a reused slot): drop the tracked state.
					ClearSlot(slot, "gone");
				}
				continue;
			}
			if (!IsSlotHuman(slot)) {
				continue;
			}
			const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(slot);
			if (steamid == 0) {
				continue;
			}
			SlotState& state = m_slots[static_cast<size_t>(slot)];
			if (state.requested && state.steamid == steamid) {
				// A load that never answered (a database hiccup) is retried every 10 seconds;
				// the core only reports a *finished* punishment load, so that is the signal.
				const bool stuck = !state.punishmentsLoaded &&
								   std::chrono::steady_clock::now() - state.requestedAt > std::chrono::seconds(10);
				if (!stuck) {
					continue;
				}
				LogError("admin_system: the load of slot " + std::to_string(slot) +
						 " did not answer within 10s - requesting it again");
			}
			// The live server does not always deliver OnClientAuthenticated (see the notes in the
			// repo), so this is the reliable path - and it also covers players that were already
			// connected when the plugin was loaded.
			state.steamid = steamid;
			state.requested = true;
			state.punishmentsLoaded = false;
			state.name = pisex::g_pPlayers->GetPlayerName(slot);
			state.requestedAt = std::chrono::steady_clock::now();
			m_core->ResetSlot(slot);
			m_core->LoadPunishments(slot, steamid, pisex::g_pPlayers->GetIpAddress(slot));
			m_core->LoadAdmin(slot, steamid, state.name);
		} catch (const std::exception& ex) {
			LogError("admin_system: reconcile of slot " + std::to_string(slot) + " failed: " + ex.what());
		} catch (...) {
			LogError("admin_system: reconcile of slot " + std::to_string(slot) + " failed");
		}
	}
}

void AdminPlugin::OnSecondTick() {
	if (m_core == nullptr) {
		return;
	}
	if (!pisex::IsGameReady()) {
		return;
	}
	// The per-second timer of the original: forget punishments and admin rights that expired.
	const ExpiryOutcome outcome = m_core->ExpireState();
	for (const ExpiryOutcome::ExpiredPunishment& expired : outcome.punishments) {
		pisex::g_pUtils->PrintToChat(expired.slot, Phrase("MuteExpired"));
		// PARITY: the original notified its listeners with admin id -1 for an expiry.
		HandlePlayerUnpunish(expired.slot, expired.type, -1);
	}
	for (const int slot : outcome.admins) {
		pisex::g_pUtils->PrintToChat(slot, Phrase("AdminExpired"));
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
		pisex::g_pUtils->PrintToChat(playerSlot, Phrase("MuteActiveChat"),
									 CStr(std::string(m_core->PunishmentReason(playerSlot, type))),
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
	if (slot >= 0 && slot < kMaxSlots) {
		m_slots[static_cast<size_t>(slot)].punishmentsLoaded = true;
	}
	// CheckPunishments(): a ban disconnects the player - immediately on connect (the original did
	// that inside the query callback) and after ban_delay when the punishments were reloaded.
	if (outcome.ban) {
		KickForBan(slot, outcome.forced);
	}
}

void AdminPlugin::KickForBan(int32_t slot, bool delayed) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	if (!__s2sdk_KickClient) {
		return; // no s2sdk: the punishment is stored, the disconnect has to happen elsewhere
	}
	// KickClient is g_pEngineServer->DisconnectClient with no validation of its own: the slot has
	// to be checked at the moment of the call. The punishment load is asynchronous, so the player
	// may already be gone (or gone and replaced) by the time this runs - a ban that nobody is
	// carrying is enforced again on the next connect anyway - and the ban_delay timer
	// additionally survives a map change. The original had neither problem (Metamod kept its
	// client objects alive). pisex::g_pPlayers->IsConnected is the "there is a live client in
	// this slot" guard of pisexcompat (map lifecycle included); it deliberately does not require
	// a controller, so a player who is still connecting is kicked as well.
	if (!delayed || m_config.ban_delay <= 0) {
		if (pisex::g_pPlayers->IsConnected(slot)) {
			s2sdk::KickClient(slot, s2sdk::NetworkDisconnectionReason::Kickbanadded, plg::string());
		}
		return;
	}
	pisex::g_pUtils->CreateTimer(static_cast<float>(m_config.ban_delay), [slot] {
		if (__s2sdk_KickClient && pisex::g_pPlayers->IsConnected(slot)) {
			s2sdk::KickClient(slot, s2sdk::NetworkDisconnectionReason::Kickbanadded, plg::string());
		}
		return -1.0f; // one shot
	});
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
	pisex::ErrorLog("%.*s", static_cast<int>(message.size()), message.data());
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
		const std::string live = pisex::g_pPlayers->GetPlayerName(adminRef);
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
	const std::string target = pisex::g_pPlayers->GetPlayerName(slot);
	const std::string duration = FormatTime(time == 0 ? 0 : static_cast<int64_t>(std::time(nullptr)) + time);
	if (m_config.notify_type == 1) {
		pisex::g_pUtils->PrintToChat(slot, Phrase(message), CStr(duration), CStr(reason), CStr(admin));
		if (adminRef >= 0 && adminRef < kMaxSlots) {
			const char* adminMessage = nullptr;
			switch (type) {
			case RT_BAN: adminMessage = "NewBanMessageAdmin"; break;
			case RT_MUTE: adminMessage = "NewMuteMessageAdmin"; break;
			case RT_GAG: adminMessage = "NewGagMessageAdmin"; break;
			case RT_SILENCE: adminMessage = "NewSilenceMessageAdmin"; break;
			default: return;
			}
			pisex::g_pUtils->PrintToChat(adminRef, Phrase(adminMessage), CStr(target), CStr(duration), CStr(reason));
		}
		return;
	}
	pisex::g_pUtils->PrintToChatAll(Phrase(message), CStr(admin), CStr(target), CStr(duration), CStr(reason));
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
	pisex::g_pUtils->PrintToChatAll(Phrase(message), CStr(AdminName(adminRef)),
									CStr(pisex::g_pPlayers->GetPlayerName(slot)));
}

// --- punishment flows ------------------------------------------------------------------------

bool AdminPlugin::AddPlayerPunishment(const PlayerIdentity& player, int32_t type, int32_t time, const std::string& reason,
									  int32_t adminRef, bool notify, bool db) {
	if (m_core == nullptr) {
		return false;
	}
	if (notify) {
		// PARITY: the original sent the notification before the INSERT.
		SendPunishmentNotification(player.slot, type, time, reason, adminRef);
	}
	const bool applied = m_core->Punish(player, type, time, reason, adminRef);
	if (applied && type == RT_BAN) {
		KickForBan(player.slot, true); // AddPunishment kicked after ban_delay
	}
	(void)db;
	return applied;
}

bool AdminPlugin::AddOfflinePlayerPunishment(const std::string& steamid64, const std::string& name, int32_t type,
											 int32_t time, const std::string& reason, int32_t adminRef) {
	if (m_core == nullptr) {
		return false;
	}
	m_core->PunishOffline(steamid64, name, type, time, reason, adminRef);
	return true;
}

bool AdminPlugin::TryAddPunishment(const PlayerIdentity& player, int32_t type, int32_t time, const std::string& reason,
								   int32_t adminRef, bool notify) {
	if (m_core == nullptr) {
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
		KickForBan(player.slot, true);
	}
	return applied;
}

void AdminPlugin::TryAddOfflinePunishment(const std::string& steamid64, const std::string& name, int32_t type,
										  int32_t time, const std::string& reason, int32_t adminRef) {
	if (m_core != nullptr) {
		m_core->TryPunishOffline(steamid64, name, type, time, reason, adminRef);
	}
}

void AdminPlugin::RemovePlayerPunishment(int32_t slot, int32_t type, int32_t adminRef, bool notify) {
	if (m_core == nullptr) {
		return;
	}
	if (notify) {
		SendUnpunishmentNotification(slot, type, adminRef);
	}
	m_core->TryUnpunish(slot, type, adminRef);
}

void AdminPlugin::TryRemovePunishment(int32_t slot, int32_t type, int32_t adminRef) {
	if (m_core != nullptr) {
		m_core->TryUnpunish(slot, type, adminRef);
	}
}

void AdminPlugin::RemoveOfflinePlayerPunishment(const std::string& steamid64, int32_t type, int32_t adminRef) {
	if (m_core != nullptr) {
		m_core->UnpunishOffline(steamid64, type, adminRef);
	}
}

// --- the admin menu / reload helpers ----------------------------------------------------------

bool AdminPlugin::IsAdminSlot(int32_t slot) const {
	return m_core != nullptr && m_core->IsAdmin(slot);
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
	LoadPhrasesFiles();
	LoadSortingIni();
	if (m_core != nullptr) {
		m_core->SetConfig(m_config);
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		// The rights of the online players are read again with the new flag mapping.
		if (const uint64_t steamid = SlotSteamId(slot); steamid != 0 && m_core != nullptr) {
			m_core->LoadAdmin(slot, steamid, pisex::g_pPlayers->GetPlayerName(slot));
		}
	}
}

// --- admin maintenance (the AddPlayerAdmin / RemovePlayerAdmin exports) -------------------------

const AdminRecord* AdminPlugin::FindAdmin(int32_t slot) const {
	return m_core == nullptr ? nullptr : m_core->FindAdmin(slot);
}

void AdminPlugin::AddAdmin(std::string name, std::string steamid64, std::string flags, int immunity, int expireSeconds,
						   int64_t groupId, std::string comment, bool db) {
	if (m_core == nullptr || steamid64.empty()) {
		return;
	}
	m_core->AddAdmin(std::move(name), std::move(steamid64), std::move(flags), immunity, expireSeconds, groupId,
					 std::move(comment), db);
}

void AdminPlugin::RemoveAdmin(const std::string& steamid64, bool db) {
	if (m_core != nullptr) {
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
									  " db_pending=" + std::to_string(m_db->Pending())));
	int humans = 0;
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		try {
			if (!pisex::g_pPlayers->IsConnected(slot) || pisex::g_pPlayers->IsFakeClient(slot)) {
				continue;
			}
			const uint64_t steamid = SlotSteamId(slot);
			if (steamid == 0) {
				continue;
			}
			++humans;
			const AdminRecord* admin = m_core->FindAdmin(slot);
			std::string line = "admin_system: slot " + std::to_string(slot) + ": steamid " + std::to_string(steamid) +
							   " name='" + pisex::g_pPlayers->GetPlayerName(slot) + "'";
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
		return 1.0f;
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
		return plg::string(pisex::g_pPlayers->GetPlayerName(slot));
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
