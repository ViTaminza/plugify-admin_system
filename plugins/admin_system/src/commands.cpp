// admin_system — the console/chat commands of the original cs2-admin_system (admin_system.cpp):
//
//   console   mm_ban, mm_mute, mm_gag, mm_silence, mm_unban, mm_unmute, mm_ungag, mm_unsilence,
//             mm_add_admin, mm_remove_admin, mm_add_group, mm_remove_group,
//             mm_as_reload_config, mm_as_reload_admin, mm_as_reload_punish, mm_admin, css_admin
//   chat      !admin, !ban, !mute, !gag, !silence, !unban, !unmute, !ungag, !unsilence,
//             !add_admin, !remove_admin, !add_group, !remove_group
//   new       as_status (RCON report: per human slot the steamid, admin state, flags, immunity,
//             group and active punishments; server console / RCON only)
//
// The parsing follows TotalCommand()/AddNewAdmin() of the original: the target is a live slot
// number, a name fragment or a steamid64, and the reason is everything after the time argument.
// pisexcompat's RegCommand hands the callback the registered spelling as arg[0] plus the
// arguments, exactly like the Metamod CCommand did.
//
// Deliberate differences from the original (small, all on purpose):
//   * the argument-count checks use the documented usage ("<target> <time> <reason>" = 4 tokens);
//     the original demanded one token more for chat calls and removed the last character of the
//     reason, which was an artifact of the raw say-command content it received;
//   * the reload commands cannot re-open the database (the connection is fixed at plugin start),
//     everything else is reloaded.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include <s2sdk/s2sdk.hpp>

#include <pisex/pisex.hpp>

#include "admin_plugin.hpp"

namespace admin_system {
namespace {

void Reply(int32_t admin, bool console, const char* text) {
	// The original printed to the server console for the console caller and to the chat for a
	// player; pisexcompat's PrintToConsole does both (a negative slot prints the server console).
	if (console) {
		pisex::g_pUtils->PrintToConsole(admin, "%s", text);
	} else {
		pisex::g_pUtils->PrintToChat(admin, "%s", text);
	}
}

bool HasPermission(int32_t admin, const char* permission) {
	// Console (-1) always passes, like the original's HasPermission.
	const AdminCore* core = g_adminPlugin.CoreOrNull();
	return core == nullptr || admin == -1 || core->HasPermission(admin, permission);
}

bool OnlyDigits(std::string_view text) {
	if (text.empty()) {
		return false;
	}
	return std::all_of(text.begin(), text.end(),
					   [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string Lowered(std::string_view text) {
	std::string result(text);
	std::transform(result.begin(), result.end(), result.begin(),
				   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return result;
}

// The player lookup of the original's TotalCommand/AddNewAdmin: a live slot (up to two digits), a
// matching steamid64, or a case-insensitive substring of the current name.
int32_t FindTarget(const std::string& arg) {
	if (arg.empty()) {
		return -1;
	}
	const bool digits = OnlyDigits(arg);
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (!pisex::g_pPlayers->IsConnected(slot) || pisex::g_pPlayers->IsFakeClient(slot)) {
			continue;
		}
		const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(slot);
		if (steamid == 0) {
			continue;
		}
		if (digits) {
			if (arg.size() <= 2 && std::atoi(arg.c_str()) == slot) {
				return slot;
			}
			try {
				if (steamid == std::stoull(arg)) {
					return slot;
				}
			} catch (const std::exception&) {
				// a long number that is not a steamid64
			}
		} else {
			const char* name = pisex::g_pPlayers->GetPlayerName(slot);
			if (name != nullptr && *name != '\0' && Lowered(name).find(Lowered(arg)) != std::string::npos) {
				return slot;
			}
		}
	}
	return -1;
}

// Everything after the time argument, without the quotes the client may have typed.
std::string ExtractReason(const pisex::CommandArgs& args) {
	std::string reason = args.ArgS();
	const char* timeArg = args.Arg(2);
	reason.erase(std::remove(reason.begin(), reason.end(), '"'), reason.end());
	if (timeArg != nullptr && *timeArg != '\0') {
		if (const std::size_t pos = reason.find(timeArg); pos != std::string::npos) {
			reason.erase(0, pos + std::string(timeArg).size());
			if (!reason.empty() && reason.front() == ' ') {
				reason.erase(0, 1);
			}
		}
	}
	return reason;
}

void PrintUsage(int32_t admin, bool console, const char* phraseKey, const char* usage, const char* command) {
	if (console) {
		Reply(admin, true, pisex::FormatString("[Admin System] Usage: %s%s", command, usage).c_str());
	} else {
		pisex::g_pUtils->PrintToChat(admin, g_adminPlugin.Phrase(phraseKey), command);
	}
}

void PlayerNotFound(int32_t admin, bool console) {
	if (console) {
		Reply(admin, true, "[Admin System] Player not found\n");
	} else {
		pisex::g_pUtils->PrintToChat(admin, g_adminPlugin.Phrase("PlayerNotFound"));
	}
}

} // namespace

// --- punishment commands (TotalCommand of the original) ------------------------------------

void CommandPunish(int32_t admin, int32_t type, const char* permission, const pisex::CommandArgs& args) {
	const bool console = admin == -1;
	if (admin != -1 && !HasPermission(admin, permission)) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	if (args.ArgC() < 4) {
		PrintUsage(admin, console, "UsagePunish", " <userid|steamid> <time> <reason>\n", args.Arg(0));
		return;
	}
	const std::string target = args.Arg(1) != nullptr ? args.Arg(1) : "";
	const int time = std::atoi(args.Arg(2) != nullptr ? args.Arg(2) : "0");
	const std::string reason = ExtractReason(args);

	const int32_t slot = FindTarget(target);
	if (slot >= 0) {
		// The original only confirmed the punishment when the immunity rules allowed it.
		if (g_adminPlugin.TryAddPunishment(g_adminPlugin.IdentityOf(slot), type, time, reason, admin, true)) {
			Reply(admin, console, "[Admin System] Player punished\n");
		}
		return;
	}
	if (OnlyDigits(target) && target.size() >= 17) {
		g_adminPlugin.TryAddOfflinePunishment(target, "undefined", type, time, reason, admin);
		Reply(admin, console, "[Admin System] Player punished\n");
		return;
	}
	PlayerNotFound(admin, console);
}

void CommandUnpunish(int32_t admin, int32_t type, const char* permission, const pisex::CommandArgs& args) {
	const bool console = admin == -1;
	if (admin != -1 && !HasPermission(admin, permission)) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	if (args.ArgC() < 2) {
		PrintUsage(admin, console, "UsageUnPunish", " <userid|steamid>\n", args.Arg(0));
		return;
	}
	const std::string target = args.Arg(1) != nullptr ? args.Arg(1) : "";
	const int32_t slot = FindTarget(target);
	if (slot >= 0) {
		g_adminPlugin.TryRemovePunishment(slot, type, admin);
		Reply(admin, console, "[Admin System] Player unpunished\n");
		return;
	}
	if (OnlyDigits(target) && target.size() >= 17) {
		g_adminPlugin.RemoveOfflinePlayerPunishment(target, type, admin);
		Reply(admin, console, "[Admin System] Player unpunished\n");
		return;
	}
	PlayerNotFound(admin, console);
}

// --- admin commands (AddNewAdmin of the original) ------------------------------------------

void CommandAddAdmin(int32_t admin, const pisex::CommandArgs& args, bool remove, bool console) {
	if (admin != -1 && !HasPermission(admin, remove ? "@admin/remove" : "@admin/add")) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	if (remove && args.ArgC() < 2) {
		PrintUsage(admin, console, "UsageRemoveAdmin", " <userid|steamid>\n", args.Arg(0));
		return;
	}
	if (!remove && args.ArgC() < 4) {
		PrintUsage(admin, console, "UsageAddAdmin",
				   " <userid|steamid> <name> <flags> <immunity> <time> <?group> <?comment>\n", args.Arg(0));
		return;
	}
	const std::string target = args.Arg(1) != nullptr ? args.Arg(1) : "";
	const int32_t slot = FindTarget(target);
	std::string steamid;
	if (slot >= 0) {
		steamid = std::to_string(g_adminPlugin.SlotSteamId(slot));
	} else if (OnlyDigits(target) && target.size() >= 17) {
		steamid = target; // the original accepted a raw steamid64 too
	} else {
		PlayerNotFound(admin, console);
		return;
	}
	if (remove) {
		g_adminPlugin.RemoveAdmin(steamid, true);
		Reply(admin, console, "[Admin System] Admin removed\n");
		return;
	}
	g_adminPlugin.AddAdmin(args.Arg(2) != nullptr ? args.Arg(2) : "", steamid, args.Arg(3) != nullptr ? args.Arg(3) : "",
						   args.Arg(4) != nullptr ? std::atoi(args.Arg(4)) : 0,
						   args.Arg(5) != nullptr ? std::atoi(args.Arg(5)) : 0,
						   args.ArgC() > 6 ? std::atoi(args.Arg(6)) : 0, args.ArgC() > 7 ? args.Arg(7) : "", true);
	Reply(admin, console, "[Admin System] Admin added\n");
}

void AddGroupCore(int32_t admin, const char* name, const char* flags, int immunity, bool console) {
	if (g_adminPlugin.CoreOrNull() == nullptr) {
		return;
	}
	g_adminPlugin.Core().AddGroup(name != nullptr ? name : "", flags != nullptr ? flags : "", immunity,
								  [admin, console](int64_t groupId) {
									  // GroupAdded of the original (the id comes back from the insert).
									  if (console) {
										  Reply(admin, true,
												pisex::FormatString("[Admin System] Group added (ID: %lld)\n",
																	static_cast<long long>(groupId))
													.c_str());
									  } else {
										  pisex::g_pUtils->PrintToChat(admin, g_adminPlugin.Phrase("GroupAdded"),
																	   static_cast<int>(groupId));
									  }
								  });
}

void CommandAddGroup(int32_t admin, const pisex::CommandArgs& args, bool console) {
	if (admin != -1 && !HasPermission(admin, "@admin/add_group")) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	if (args.ArgC() < 4) {
		PrintUsage(admin, console, "UsageAddGroup", " <name> <flags> <immunity>\n", args.Arg(0));
		return;
	}
	AddGroupCore(admin, args.Arg(1), args.Arg(2), args.Arg(3) != nullptr ? std::atoi(args.Arg(3)) : 0, console);
}

void CommandRemoveGroup(int32_t admin, const pisex::CommandArgs& args, bool console) {
	if (admin != -1 && !HasPermission(admin, "@admin/remove_group")) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	if (args.ArgC() < 2) {
		PrintUsage(admin, console, "UsageRemoveGroup", " <id/name>\n", args.Arg(0));
		return;
	}
	if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
		core->RemoveGroup(args.Arg(1) != nullptr ? args.Arg(1) : "");
	}
	Reply(admin, console, "[Admin System] Group removed\n");
}

// --- reload commands --------------------------------------------------------------------------

void CommandReloadConfig(int32_t admin) {
	const bool console = admin == -1;
	if (admin != -1 && !HasPermission(admin, "@admin/reload_config")) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	g_adminPlugin.ReloadConfig();
	Reply(admin, console, "[Admin System] Config reloaded\n");
}

void CommandReloadAdmin(int32_t admin, const pisex::CommandArgs& args) {
	const bool console = admin == -1;
	if (admin != -1 && !HasPermission(admin, "@admin/reload_admin")) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	if (args.ArgC() < 2) {
		PrintUsage(admin, console, "UsageReloadAdmin", " <steamid64>\n", args.Arg(0));
		return;
	}
	uint64_t value = 0;
	try {
		value = std::stoull(args.Arg(1) != nullptr ? args.Arg(1) : "");
	} catch (const std::exception&) {
		PlayerNotFound(admin, console);
		return;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (g_adminPlugin.SlotSteamId(slot) != value) {
			continue;
		}
		if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
			core->LoadAdmin(slot, value, pisex::g_pPlayers->GetPlayerName(slot));
		}
		return;
	}
	PlayerNotFound(admin, console);
}

void CommandReloadPunish(int32_t admin, const pisex::CommandArgs& args) {
	const bool console = admin == -1;
	if (admin != -1 && !HasPermission(admin, "@admin/reload_punish")) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	if (args.ArgC() < 2) {
		PrintUsage(admin, console, "UsageReloadPunish", " <steamid64>\n", args.Arg(0));
		return;
	}
	uint64_t value = 0;
	try {
		value = std::stoull(args.Arg(1) != nullptr ? args.Arg(1) : "");
	} catch (const std::exception&) {
		PlayerNotFound(admin, console);
		return;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (g_adminPlugin.SlotSteamId(slot) != value) {
			continue;
		}
		if (AdminCore* core = g_adminPlugin.CoreOrNull()) {
			// OnReloadPunish of the original: clear the slot, then force the reload.
			core->ResetSlot(slot);
			core->ReloadPunishments(slot, value, pisex::g_pPlayers->GetIpAddress(slot));
		}
		return;
	}
	PlayerNotFound(admin, console);
}

// --- registration ------------------------------------------------------------------------------

namespace {

// The punish/unpunish dispatcher of mm_* (console) and !* (chat): the command name picks the type
// and the permission, exactly like PunishCommand/UnPunishCommand of the original.
struct PunishMapping {
	const char* command;
	int32_t type;
	const char* permission;
	bool unpunish;
};

constexpr PunishMapping kPunishMappings[] = {
	{"mm_ban", RT_BAN, "@admin/ban", false},    {"mm_mute", RT_MUTE, "@admin/mute", false},
	{"mm_gag", RT_GAG, "@admin/gag", false},    {"mm_silence", RT_SILENCE, "@admin/silence", false},
	{"mm_unban", RT_BAN, "@admin/unban", true}, {"mm_unmute", RT_MUTE, "@admin/unmute", true},
	{"mm_ungag", RT_GAG, "@admin/ungag", true}, {"mm_unsilence", RT_SILENCE, "@admin/unsilence", true},
	{"!ban", RT_BAN, "@admin/ban", false},      {"!mute", RT_MUTE, "@admin/mute", false},
	{"!gag", RT_GAG, "@admin/gag", false},      {"!silence", RT_SILENCE, "@admin/silence", false},
	{"!unban", RT_BAN, "@admin/unban", true},   {"!unmute", RT_MUTE, "@admin/unmute", true},
	{"!ungag", RT_GAG, "@admin/ungag", true},   {"!unsilence", RT_SILENCE, "@admin/unsilence", true},
};

const PunishMapping* FindMapping(std::string_view spelling) {
	for (const PunishMapping& mapping : kPunishMappings) {
		if (spelling == mapping.command) {
			return &mapping;
		}
	}
	return nullptr;
}

bool PunishCommand(int32_t slot, const char* content) {
	try {
		pisex::CommandArgs args(content);
		const PunishMapping* mapping = FindMapping(args.Arg(0) != nullptr ? args.Arg(0) : "");
		if (mapping == nullptr) {
			return false;
		}
		if (mapping->unpunish) {
			CommandUnpunish(slot, mapping->type, mapping->permission, args);
		} else {
			CommandPunish(slot, mapping->type, mapping->permission, args);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in a punishment command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in a punishment command");
	}
	return true;
}

bool AdminMenuCommand(int32_t slot, const char*) {
	try {
		g_adminPlugin.ShowAdminMenu(slot);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the admin menu command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the admin menu command");
	}
	return true;
}

bool AddAdminCommand(int32_t slot, const char* content, bool remove, bool console) {
	try {
		pisex::CommandArgs args(content);
		CommandAddAdmin(slot, args, remove, console);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the add/remove admin command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the add/remove admin command");
	}
	return true;
}

bool GroupCommand(int32_t slot, const char* content, bool remove, bool console) {
	try {
		pisex::CommandArgs args(content);
		if (remove) {
			CommandRemoveGroup(slot, args, console);
		} else {
			CommandAddGroup(slot, args, console);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the group command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the group command");
	}
	return true;
}

bool ReloadConfigCommand(int32_t slot, const char*) {
	try {
		CommandReloadConfig(slot);
	} catch (...) {
		LogError("admin_system: unknown exception in mm_as_reload_config");
	}
	return true;
}

bool ReloadAdminCommand(int32_t slot, const char* content) {
	try {
		pisex::CommandArgs args(content);
		CommandReloadAdmin(slot, args);
	} catch (...) {
		LogError("admin_system: unknown exception in mm_as_reload_admin");
	}
	return true;
}

bool ReloadPunishCommand(int32_t slot, const char* content) {
	try {
		pisex::CommandArgs args(content);
		CommandReloadPunish(slot, args);
	} catch (...) {
		LogError("admin_system: unknown exception in mm_as_reload_punish");
	}
	return true;
}

s2sdk::ResultType AsStatusCommand(int32_t caller, s2sdk::ConCommandContext context,
								  const plg::vector<plg::string>& args) {
	(void)args;
	// Server console / RCON only (caller -1). The s2sdk chat path executes console commands by
	// name without checking their flags, and the gate the original's status module used for the
	// player list ("@admin/status") is a default_permissions entry for regular players, so
	// "!as_status" would otherwise leak steamids, admin flags, immunity, groups and punishments
	// to any client.
	if (caller != -1) {
		s2sdk::ReplyToCommand(context, caller,
							  plg::string("admin_system: as_status is available on the server console only"));
		return s2sdk::ResultType::Handled;
	}
	try {
		g_adminPlugin.PrintStatus(caller, context);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in as_status: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in as_status");
	}
	return s2sdk::ResultType::Handled;
}

// True while "as_status" is registered with s2sdk (see RegisterCommands/UnregisterCommands).
bool g_statusCommandRegistered = false;

} // namespace

void RegisterCommands() {
	// mm_ban / mm_mute / ... / !ban / !mute / ... share one handler.
	std::vector<std::string> consolePunish;
	std::vector<std::string> chatPunish;
	for (const PunishMapping& mapping : kPunishMappings) {
		if (mapping.command[0] == '!') {
			chatPunish.emplace_back(mapping.command);
		} else {
			consolePunish.emplace_back(mapping.command);
		}
	}
	// The mm_* names were CON_COMMAND_EXTERN (server console only) in the original; the chat
	// spellings stay client-executable.
	pisex::g_pUtils->RegServerCommand(consolePunish, &PunishCommand);
	pisex::g_pUtils->RegCommand({}, chatPunish, &PunishCommand);

	// mm_admin / css_admin (console) and !admin (chat): the admin menu.
	pisex::g_pUtils->RegCommand({"mm_admin", "css_admin"}, {"!admin"}, &AdminMenuCommand);

	// Admin management.
	pisex::g_pUtils->RegServerCommand({"mm_add_admin"},
									  [](int32_t slot, const char* content) { return AddAdminCommand(slot, content, false, true); });
	pisex::g_pUtils->RegServerCommand({"mm_remove_admin"},
									  [](int32_t slot, const char* content) { return AddAdminCommand(slot, content, true, true); });
	pisex::g_pUtils->RegCommand({}, {"!add_admin"},
								[](int32_t slot, const char* content) { return AddAdminCommand(slot, content, false, false); });
	pisex::g_pUtils->RegCommand({}, {"!remove_admin"},
								[](int32_t slot, const char* content) { return AddAdminCommand(slot, content, true, false); });

	// Groups.
	pisex::g_pUtils->RegServerCommand({"mm_add_group"},
									  [](int32_t slot, const char* content) { return GroupCommand(slot, content, false, true); });
	pisex::g_pUtils->RegServerCommand({"mm_remove_group"},
									  [](int32_t slot, const char* content) { return GroupCommand(slot, content, true, true); });
	pisex::g_pUtils->RegCommand({}, {"!add_group"},
								[](int32_t slot, const char* content) { return GroupCommand(slot, content, false, false); });
	pisex::g_pUtils->RegCommand({}, {"!remove_group"},
								[](int32_t slot, const char* content) { return GroupCommand(slot, content, true, false); });

	// The reload commands.
	pisex::g_pUtils->RegServerCommand({"mm_as_reload_config"}, &ReloadConfigCommand);
	pisex::g_pUtils->RegServerCommand({"mm_as_reload_admin"}, &ReloadAdminCommand);
	pisex::g_pUtils->RegServerCommand({"mm_as_reload_punish"}, &ReloadPunishCommand);

	// as_status (RCON): the status report of this port.
	if (!__s2sdk_AddConsoleCommand) {
		LogError("admin_system: the s2sdk console command binding is not available - as_status is not registered");
		return;
	}
	if (s2sdk::AddConsoleCommand("as_status",
								 "Prints the admin state of every connected human player (admin, flags, immunity, group, punishments)",
								 s2sdk::ConVarFlag::None, &AsStatusCommand, s2sdk::HookMode::Pre)) {
		g_statusCommandRegistered = true;
		LogInfo("admin_system: registered the as_status console command");
	} else {
		LogError("admin_system: registering the as_status console command failed");
	}
}

void UnregisterCommands() {
	// as_status was registered with s2sdk directly (not through pisexcompat), so pisex::Shutdown()
	// does not know about it: without this the engine keeps a listener pointing into the unloaded
	// module and the next "as_status" (RCON) crashes the server.
	if (!g_statusCommandRegistered) {
		return;
	}
	g_statusCommandRegistered = false;
	if (!__s2sdk_RemoveCommand) {
		return;
	}
	try {
		s2sdk::RemoveCommand("as_status", &AsStatusCommand);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: removing as_status failed: ") + ex.what());
	} catch (...) {
		LogError("admin_system: removing as_status failed");
	}
}

} // namespace admin_system
