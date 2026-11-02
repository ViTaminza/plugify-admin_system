// Runtime smoke test of plugins/admin_system: the real plugin translation units driven through
// their whole lifecycle with EVERY s2sdk and hudmenu binding null.
//
// This is the PITFALLS A1/A7 path the plugin must survive on a server where s2sdk is missing or
// only partially loaded: no client, menu, command, event or timer binding exists, so every
// registration has to degrade to a logged no-op, `OnPluginStart` must still return Ok (returning
// Failed would stop every Plugify plugin), the exported methods must answer their neutral values,
// and `OnPluginEnd` must tear down nothing without crashing or touching a client.
//
// The link set is the real one: the plugin sources, the *empty* s2sdk/hudmenu binding globals
// (s2sdk/s2sdk.cpp defines every __s2sdk_* slot as nullptr), admin_core and plgdb. The plugin's
// SQLite fail-soft path runs for real: with no plugify data directory the database file lands in
// the working directory, so the test switches to a temporary directory first and removes it again.
//
// Run by ./test.sh (build/tests/admin_runtime_tests).

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

#include <plg/plugin.hpp>

#include "admin_config.hpp"
#include "admin_guards.hpp"
#include "admin_menus.hpp"
#include "admin_plugin.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(expr)                                                                                \
	do {                                                                                           \
		++g_checks;                                                                                \
		if (!(expr)) {                                                                             \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                            \
		}                                                                                          \
	} while (false)

// A temporary working directory: the plugin's SQLite fallback is relative to the cwd when the host
// provides no data directory, and the test must not leave files in the source tree.
struct TempWorkingDir {
	std::filesystem::path previous;
	std::filesystem::path path;

	TempWorkingDir() {
		static int counter = 0;
		previous = std::filesystem::current_path();
		path = std::filesystem::temp_directory_path() /
			   ("admin_system_runtime_tests_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		std::filesystem::current_path(path, ec);
	}
	~TempWorkingDir() {
		std::error_code ec;
		std::filesystem::current_path(previous, ec);
		std::filesystem::remove_all(path, ec);
	}
	TempWorkingDir(const TempWorkingDir&) = delete;
	TempWorkingDir& operator=(const TempWorkingDir&) = delete;
};

} // namespace

// The exported methods of plugin.cpp (the IAdminApi surface).
extern "C" float GetPluginVersion();
extern "C" plg::string GetTranslation(const plg::string& key);
extern "C" plg::vector<plg::string> GetDatabaseConfig();
extern "C" bool IsCoreLoaded();
extern "C" bool IsAdmin(int32_t slot);
extern "C" bool IsClientAdminDataReady(int32_t slot, uint64_t steamid);
extern "C" bool HasPermission(int32_t slot, const plg::string& permission);
extern "C" int32_t GetMessageType();
extern "C" int32_t GetImmunityType();
extern "C" int64_t GetPlayerPunishmentExpired(int32_t slot, int32_t type);
extern "C" plg::string GetPlayerPunishmentReason(int32_t slot, int32_t type);
extern "C" plg::string GetAdminName(int32_t slot);
extern "C" int32_t GetAdminGroupID(int32_t slot);

int main() {
	TempWorkingDir workingDir;

	// --- the guards answer "no" while no map is up and no binding exists ------------------------
	CHECK(!admin_system::IsMapReady());
	CHECK(!admin_system::MapListenerBindingsUsable());
	CHECK(!admin_system::MenuBindingsAvailable());
	CHECK(!admin_system::HasClient(0));
	CHECK(!admin_system::HasController(0));
	CHECK(!admin_system::HasPawn(0));
	CHECK(!admin_system::IsFakeClientSlot(0));
	CHECK(!admin_system::IsAuthorizedSlot(0));
	CHECK(admin_system::ClientSteamID64(0) == 0);
	CHECK(admin_system::ClientName(0).empty());
	CHECK(admin_system::ClientIp(0).empty());
	CHECK(!admin_system::KickClientSlot(0, 0));
	admin_system::SetClientNameSafe(0, "nobody"); // must be a no-op, not a crash
	admin_system::OwnerIdentity owner;
	CHECK(!admin_system::CaptureOwner(0, owner));
	CHECK(!admin_system::OwnerMatches(admin_system::OwnerIdentity{}));

	// The printers are no-ops without a map (they are called from the exported methods).
	admin_system::PrintChat(0, "%s", "hello");
	admin_system::PrintChatAll("%s", "hello");
	admin_system::PrintConsole(0, "%s", "hello");
	admin_system::PrintConsole(-1, "%s", "hello"); // the server-console branch (plg::Log is null)
	CHECK(admin_system::FormatText("%d-%s", 3, "x") == "3-x");

	// The colour table is byte-identical to pisexcompat's.
	CHECK(admin_system::Colorize("{RED}x{DEFAULT}") == std::string("\x02x\x01"));
	CHECK(admin_system::StripColorTags("{RED}x{DEFAULT}") == "x");
	// U+2029 (e2 80 a9) is the chat line break; concatenated so the hex escape cannot swallow the
	// trailing "b" (a hex digit).
	CHECK(admin_system::Colorize("a\\nb") == std::string("a") + "\xe2\x80\xa9" + "b");
	CHECK(admin_system::StripColorTags("a\\nb") == "a\nb");

	// --- the exported values before the plugin started ------------------------------------------
	CHECK(GetPluginVersion() == 1.2f);
	CHECK(!IsCoreLoaded());
	CHECK(!IsAdmin(0));
	CHECK(!IsClientAdminDataReady(0, 0));
	CHECK(!HasPermission(0, "anything"));
	CHECK(GetMessageType() == 0 || GetMessageType() == 1); // the Config defaults
	CHECK(GetImmunityType() >= 0);
	CHECK(GetPlayerPunishmentExpired(0, 0) == -1);
	CHECK(GetPlayerPunishmentReason(0, 0).empty());
	CHECK(std::string_view(GetAdminName(-1)) == "Console");
	CHECK(GetAdminGroupID(0) == -1);
	// GetTranslation answers the key itself when the phrases are not loaded (like the original).
	CHECK(std::string_view(GetTranslation(plg::string("NoSuchPhrase"))) == "NoSuchPhrase");

	// --- the lifecycle with every binding null -------------------------------------------------
	admin_system::AdminPlugin& plugin = admin_system::g_adminPlugin;
	const plg::PluginResult started = plugin.OnPluginStart();
	CHECK(started.GetCode() == plg::PluginCode::Ok); // never Failed (PITFALLS A1)

	// The fail-soft database: no databases.cfg and no plugify directories, so the plugin opened a
	// local SQLite file (relative to the working directory) and created the schema.
	const plg::vector<plg::string> dbConfig = GetDatabaseConfig();
	CHECK(dbConfig.size() == 7);
	if (dbConfig.size() == 7) {
		CHECK(std::string_view(dbConfig[0]) == "sqlite");
		CHECK(std::string_view(dbConfig[1]).find("admin_system.sqlite3") != std::string_view::npos);
		CHECK(dbConfig[3].empty()); // no user for SQLite
		CHECK(dbConfig[4].empty()); // no password for SQLite
		CHECK(std::string_view(dbConfig[6]) == "as_"); // the default table prefix
	}
	// The phrases file is missing: every key prints as itself.
	CHECK(std::string_view(plugin.Phrase("MainTitle")) == "MainTitle");

	// No map is up, so the startup work (and the CoreLoaded broadcast) has not run.
	CHECK(!IsCoreLoaded());
	// A few frames with the gate closed: nothing may be touched, no exception may escape.
	for (int i = 0; i < 3; ++i) {
		CHECK(plugin.OnPluginUpdate(std::chrono::milliseconds(16)).GetCode() == plg::PluginCode::Ok);
	}
	CHECK(!admin_system::IsMapReady());
	// The core is up (SQLite), so the permission checks are authoritative and deny a slot without
	// a record; the console always passes.
	CHECK(!IsAdmin(0));
	CHECK(!HasPermission(0, "@admin/root"));
	CHECK(HasPermission(-1, "@admin/root"));
	CHECK(!IsClientAdminDataReady(0, 0));

	// Exports that touch the menu layer must not create anything without the bindings.
	admin_system::g_adminPlugin.ShowAdminMenu(0);
	admin_system::g_adminPlugin.CloseAdminMenu(0);
	CHECK(!admin_system::IsMenuOpen(0));
	// The chat listener with no client: the own-reason capture is off, the gag check has no core
	// state, so the message is allowed through.
	CHECK(plugin.OnChatPre(0, "hello", false));
	CHECK(plugin.OnChatPre(70, "hello", false)); // out of range: allowed, not a crash

	// --- the teardown --------------------------------------------------------------------------
	CHECK(plugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok);
	CHECK(!admin_system::IsMapReady());
	CHECK(plugin.CoreOrNull() == nullptr);
	// A second update after the end is harmless (the plugin object still exists).
	CHECK(plugin.OnPluginUpdate(std::chrono::milliseconds(16)).GetCode() == plg::PluginCode::Ok);
	CHECK(!IsCoreLoaded());
	// The SQLite file of the fail-soft path really was created in the working directory (proof
	// that the database path, not a disabled feature, produced the "core ready" state above).
	const bool dbFile = std::filesystem::exists(workingDir.path / "admin_system" / "admin_system.sqlite3");
	CHECK(dbFile);

	if (g_failures != 0) {
		std::printf("admin_runtime_tests: %d/%d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("admin_runtime_tests: all %d checks passed\n", g_checks);
	return 0;
}
