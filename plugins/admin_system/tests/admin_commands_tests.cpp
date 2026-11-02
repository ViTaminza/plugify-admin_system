// Tests for the command layer of plugins/admin_system (src/commands.cpp) with fake s2sdk bindings.
//
// The real plugin translation units are linked, the `__s2sdk_*` globals are filled with recording
// fakes and the plugin runs its normal lifecycle (config files, SQLite database, map gate), so the
// command handlers are exercised exactly as on a live server. Three safety rules are covered:
//
//   1. PITFALLS C11 - strict callers: only the server console (-1) and real slots (0..63) reach a
//      command handler; a -2/64 is swallowed without a permission check and without a reply. The
//      mm_* commands are server-console only (exactly -1), the "!ban"/"!admin" spellings are chat
//      spellings and are not dispatched from a console context.
//   2. The admin-data readiness gate: the core's in-memory record is the previous answer until the
//      SELECT for the current steamid completes. After a slot is re-used, the cached rights of the
//      old player must NOT authorize the new one (IsAdminDataReady is checked before HasPermission).
//   3. Deferred replies (the offline punishment insert and the group insert) carry the verified
//      OwnerIdentity captured when the command ran and are dropped when the slot was re-used before
//      the database answered; the server console (-1) always gets its reply.
//
// Run by ./test.sh (build/tests/admin_commands_tests).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include "admin_commands_test_config.hpp"
#include "admin_guard_test_fakes.hpp"
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

using admin_system::RT_BAN;
using admin_system::AdminPlugin;

constexpr int32_t kSlot = 3;
constexpr uint64_t kAdminSteamId = 76561198000000001ull;
constexpr uint64_t kOtherSteamId = 76561198000000002ull;

// Reloads the admin record of the test slot for a steamid and pumps the database until the answer
// is authoritative again (the core's IsAdminDataReady is exactly the signal the permission check
// uses).
bool ReloadAdminForSlot(AdminPlugin& plugin, uint64_t steamid) {
	plugin.Core().LoadAdmin(kSlot, steamid, "Admin");
	for (int i = 0; i < 400 && !plugin.Core().IsAdminDataReady(kSlot, steamid); ++i) {
		plugin.OnPluginUpdate(std::chrono::milliseconds(16));
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return plugin.Core().IsAdminDataReady(kSlot, steamid);
}

plg::vector<plg::string> Args(std::initializer_list<const char*> tokens) {
	plg::vector<plg::string> out;
	for (const char* token : tokens) {
		out.emplace_back(token);
	}
	return out;
}

} // namespace

int main() {
	using namespace admin_system;

	// The plugin reads its files from the plugify directories and writes only into its data dir.
	admin_test::TempDirs dirs;
	admin_test::WriteAdminConfigs(dirs);
	plg::GetConfigsDir = &admin_test::FakeConfigsDir;
	plg::GetDataDir = &admin_test::FakeDataDir;
	plg::Log = &admin_test::FakeLog;

	admin_test::InstallFakeBindings();

	AdminPlugin& plugin = g_adminPlugin;
	CHECK(plugin.OnPluginStart().GetCode() == plg::PluginCode::Ok);

	// Open the map gate: GetCurrentMap reports a real map, both listeners are registered by
	// InitMapGate(), so the first update after OnPluginStart proves the live map (B1).
	CHECK(!IsMapReady());
	plugin.OnPluginUpdate(std::chrono::milliseconds(16));
	CHECK(IsMapReady());
	CHECK(HasController(kSlot));
	CHECK(ClientSteamID64(kSlot) == kAdminSteamId);

	// --- make slot 3 an admin (flag "b" -> @admin/ban from the test core.ini) -------------------
	plugin.AddAdmin("Admin", std::to_string(kAdminSteamId), "b", 0, 0, 0, "", true);
	// AddAdmin inserts asynchronously and reloads the online slot; the reconcile may already have
	// answered "no rows" for this steamid before the INSERT was applied, so wait for the *rights*
	// (not just for a Ready state) and then verify the load state.
	for (int i = 0; i < 600 && !plugin.Core().HasPermission(kSlot, "@admin/ban"); ++i) {
		plugin.OnPluginUpdate(std::chrono::milliseconds(16));
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(plugin.Core().HasPermission(kSlot, "@admin/ban"));
	CHECK(plugin.Core().HasPermission(kSlot, "@admin/add_group"));
	CHECK(plugin.Core().IsAdminDataReady(kSlot, kAdminSteamId));

	admin_test::ClearOutput();

	// =============================================================================================
	// 1) PITFALLS C11: strict callers
	// =============================================================================================
	const s2sdk::ConCommandCallback mmBan = admin_test::FindCommand("mm_ban");
	const s2sdk::ConCommandCallback banChat = admin_test::FindCommand("ban");
	const s2sdk::ConCommandCallback mmAdmin = admin_test::FindCommand("mm_admin");
	const s2sdk::ConCommandCallback adminChat = admin_test::FindCommand("admin");
	CHECK(mmBan != nullptr);
	CHECK(banChat != nullptr);
	CHECK(mmAdmin != nullptr);
	CHECK(adminChat != nullptr);
	if (mmBan == nullptr || banChat == nullptr || mmAdmin == nullptr || adminChat == nullptr) {
		std::printf("admin_commands_tests: the command table is incomplete - aborting\n");
		return 1;
	}

	// The mm_* commands run only for exactly -1 (the server console); -2 and 64 are swallowed.
	CHECK(mmBan(-2, s2sdk::ConCommandContext::Console, Args({"mm_ban", "zzz", "60", "reason"})) ==
		  s2sdk::ResultType::Handled);
	CHECK(mmBan(64, s2sdk::ConCommandContext::Console, Args({"mm_ban", "zzz", "60", "reason"})) ==
		  s2sdk::ResultType::Handled);
	CHECK(admin_test::ChatOutput().empty());
	CHECK(admin_test::ServerConsoleOutput().empty());
	// The chat spelling is not dispatched from a console context (and a -2 is no caller either).
	CHECK(banChat(-2, s2sdk::ConCommandContext::Chat, Args({"ban", "zzz", "60", "reason"})) ==
		  s2sdk::ResultType::Handled);
	CHECK(banChat(-2, s2sdk::ConCommandContext::Console, Args({"ban", "zzz", "60", "reason"})) ==
		  s2sdk::ResultType::Continue);
	// The menu commands: mm_admin is not a chat spelling, "admin" is; a bogus caller is swallowed.
	CHECK(mmAdmin(-2, s2sdk::ConCommandContext::Console, Args({"mm_admin"})) == s2sdk::ResultType::Handled);
	CHECK(mmAdmin(-1, s2sdk::ConCommandContext::Chat, Args({"mm_admin"})) == s2sdk::ResultType::Continue);
	CHECK(adminChat(64, s2sdk::ConCommandContext::Chat, Args({"admin"})) == s2sdk::ResultType::Handled);
	CHECK(adminChat(-1, s2sdk::ConCommandContext::Console, Args({"admin"})) == s2sdk::ResultType::Continue);
	CHECK(admin_test::ChatOutput().empty());
	CHECK(admin_test::ServerConsoleOutput().empty());
	// Control: the server console (-1) does reach the handler and gets its reply.
	CHECK(mmBan(-1, s2sdk::ConCommandContext::Console, Args({"mm_ban", "zzz", "60", "reason"})) ==
		  s2sdk::ResultType::Handled);
	CHECK(admin_test::ServerConsoleOutput().size() == 1);
	CHECK(admin_test::LastServerConsole().find("Player not found") != std::string::npos);

	// =============================================================================================
	// 2) the admin-data readiness gate on the permission check
	// =============================================================================================
	admin_test::ClearOutput();
	// With the answer ready for the live steamid the command runs (the target is just not found).
	CHECK(banChat(kSlot, s2sdk::ConCommandContext::Chat, Args({"ban", "zzz", "60", "reason"})) ==
		  s2sdk::ResultType::Handled);
	CHECK(admin_test::ChatOutput().size() == 1);
	CHECK(admin_test::LastChat().first == kSlot);
	CHECK(admin_test::LastChat().second.find("Player not found") != std::string::npos);

	// The slot is re-used: the live steamid changes, but the core still holds the record of the
	// previous player (Ready for kAdminSteamId, no load for the new one). The cached rights must NOT
	// authorize the new occupant.
	admin_test::SetClientSteamId(kOtherSteamId);
	CHECK(!plugin.Core().IsAdminDataReady(kSlot, kOtherSteamId)); // Pending/Unknown for the new player
	CHECK(plugin.Core().HasPermission(kSlot, "@admin/ban"));      // the stale cache still says "admin"
	admin_test::ClearOutput();
	CHECK(banChat(kSlot, s2sdk::ConCommandContext::Chat, Args({"ban", "zzz", "60", "reason"})) ==
		  s2sdk::ResultType::Handled);
	CHECK(admin_test::ChatOutput().size() == 1);
	CHECK(admin_test::LastChat().second.find("You don't have permission") != std::string::npos);
	CHECK(admin_test::LastChat().second.find("Player not found") == std::string::npos);
	// Restore the verified owner and reload its record (the core still answers for the other
	// steamid at this point; LoadAdmin makes the answer authoritative for kAdminSteamId again).
	admin_test::SetClientSteamId(kAdminSteamId);
	CHECK(ReloadAdminForSlot(plugin, kAdminSteamId));

	// =============================================================================================
	// 3) deferred replies carry the verified owner
	// =============================================================================================
	// 3a) group insert, owner unchanged: the chat reply arrives.
	admin_test::ClearOutput();
	AddGroupCore(kSlot, "group_ok", "b", 1, false);
	bool groupReplied = false;
	for (int i = 0; i < 400 && !groupReplied; ++i) {
		plugin.OnPluginUpdate(std::chrono::milliseconds(16));
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		groupReplied = !admin_test::ChatOutput().empty();
	}
	CHECK(groupReplied);
	if (groupReplied) {
		CHECK(admin_test::LastChat().first == kSlot);
		CHECK(admin_test::LastChat().second.find("Group added") != std::string::npos);
	}

	// 3b) the owner changes before the insert answered: the reply is dropped.
	admin_test::ClearOutput();
	AddGroupCore(kSlot, "group_stale", "b", 1, false);
	admin_test::SetClientSteamId(kOtherSteamId);
	for (int i = 0; i < 200; ++i) {
		plugin.OnPluginUpdate(std::chrono::milliseconds(16));
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(admin_test::ChatOutput().empty());
	admin_test::SetClientSteamId(kAdminSteamId);
	// The pumps above let the reconcile reload the slot for the other steamid; make the answer
	// authoritative for the owner again before the next deferred reply is tested.
	CHECK(ReloadAdminForSlot(plugin, kAdminSteamId));

	// 3c) the server console always gets its deferred reply (no player identity involved).
	admin_test::ClearOutput();
	AddGroupCore(-1, "group_console", "b", 1, true);
	bool consoleReplied = false;
	for (int i = 0; i < 400 && !consoleReplied; ++i) {
		plugin.OnPluginUpdate(std::chrono::milliseconds(16));
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		consoleReplied = !admin_test::ServerConsoleOutput().empty();
	}
	CHECK(consoleReplied);
	if (consoleReplied) {
		CHECK(admin_test::LastServerConsole().find("Group added") != std::string::npos);
	}

	// 3d) the offline punishment reply: delivered for the verified owner ...
	admin_test::ClearOutput();
	admin_system::CommandArgs punishLine(("mm_ban " + std::to_string(kOtherSteamId) + " 60 cheating").c_str());
	CommandPunish(kSlot, RT_BAN, "@admin/ban", punishLine);
	bool punishedReplied = false;
	for (int i = 0; i < 400 && !punishedReplied; ++i) {
		plugin.OnPluginUpdate(std::chrono::milliseconds(16));
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		punishedReplied = !admin_test::ChatOutput().empty();
	}
	CHECK(punishedReplied);
	if (punishedReplied) {
		CHECK(admin_test::LastChat().first == kSlot);
	}

	// ... and dropped when the slot is re-used before the insert answered.
	admin_test::ClearOutput();
	admin_system::CommandArgs staleLine(("mm_ban " + std::to_string(kOtherSteamId) + " 60 cheating").c_str());
	CommandPunish(kSlot, RT_BAN, "@admin/ban", staleLine);
	admin_test::SetClientSteamId(kOtherSteamId);
	for (int i = 0; i < 200; ++i) {
		plugin.OnPluginUpdate(std::chrono::milliseconds(16));
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	CHECK(admin_test::ChatOutput().empty());
	admin_test::SetClientSteamId(kAdminSteamId);

	// --- teardown ------------------------------------------------------------------------------
	CHECK(plugin.OnPluginEnd().GetCode() == plg::PluginCode::Ok);
	plg::GetConfigsDir = nullptr;
	plg::GetDataDir = nullptr;
	plg::Log = nullptr;

	if (g_failures != 0) {
		std::printf("admin_commands_tests: %d/%d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("admin_commands_tests: all %d checks passed\n", g_checks);
	return 0;
}
