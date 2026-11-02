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
// s2sdk dispatches a chat "!ban ..." as the bare command "ban" (ConCommandContext::Chat) and hands
// the callback the tokenized arguments; the plugin rebuilds the Pisex content string from them
// (admin_command_args.hpp) so the ported handlers and their usage strings stay unchanged.
//
// Registration rules (PITFALLS A1/A7/C11):
//   * every registration is a plain function pointer, and the name may already exist in the engine
//     (a previous `plg reload`, another plugin): AddConsoleCommand answers false then and only
//     attaches our listener. Both RemoveCommand and RemoveCommandListener must therefore be
//     available before anything is registered, and UnregisterCommands detaches exactly what was
//     attached;
//   * the mm_* commands were CON_COMMAND_EXTERN (server console only) in the original: they are
//     registered without ClientCanExecute *and* the handler swallows any caller >= 0, so a chat
//     path can never reach them (PITFALLS C11);
//   * as_status is server-console/RCON only as well: `caller == -1` exactly (-2 is not a bypass),
//     because s2sdk's chat path executes console commands by name without checking their flags -
//     "!as_status" would otherwise leak steamids, admin flags, immunity, groups and punishments to
//     any client.
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

#include "admin_command_args.hpp"
#include "admin_guards.hpp"
#include "admin_plugin.hpp"

namespace admin_system {
namespace {

// The server console / RCON caller (admin_guards.hpp): exactly -1, never -2.
constexpr int32_t kConsoleCaller = kServerConsoleSlot;

void Reply(int32_t admin, bool console, const char* text) {
	// The original printed to the server console for the console caller and to the chat for a
	// player; PrintConsole does both (a negative slot prints the server console, tags stripped).
	if (console) {
		PrintConsole(admin, "%s", text);
	} else {
		PrintChat(admin, "%s", text);
	}
}

bool HasPermission(int32_t admin, const char* permission) {
	// Console (-1) always passes, like the original's HasPermission. Everything else is denied
	// while the core is missing: the plugin registers the commands before the database answers
	// (it retries in the background), and a null core must not pass every permission check.
	if (admin == kConsoleCaller) {
		return true;
	}
	const AdminCore* core = g_adminPlugin.CoreOrNull();
	if (core == nullptr) {
		return false;
	}
	// The in-memory record is the *previous* answer until the core's SELECT for the current steamid
	// completes: after a slot is re-used the cached rights of the old player must not authorize the
	// new one (a Pending/Failed load is not authoritative - IsAdminDataReady is the signal the
	// modules use for exactly this reason). Fail closed.
	const uint64_t steamid = ClientSteamID64(admin);
	if (steamid == 0 || !core->IsAdminDataReady(admin, steamid)) {
		return false;
	}
	return core->HasPermission(admin, permission);
}

// True (and a reply sent) while the core is missing - the database was unreachable at startup
// and the background retry has not succeeded yet. A command that changes database state must
// say so instead of confirming the silent no-op of the plugin methods.
bool CoreNotReady(int32_t admin, bool console) {
	if (g_adminPlugin.CoreOrNull() != nullptr) {
		return false;
	}
	Reply(admin, console, "[Admin System] The core is not ready (the database is unavailable)\n");
	return true;
}

bool OnlyDigits(std::string_view text) {
	if (text.empty()) {
		return false;
	}
	return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
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
		if (!HasClient(slot) || IsFakeClientSlot(slot)) {
			continue;
		}
		const uint64_t steamid = ClientSteamID64(slot);
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
			const std::string name = ClientName(slot);
			if (!name.empty() && Lowered(name).find(Lowered(arg)) != std::string::npos) {
				return slot;
			}
		}
	}
	return -1;
}

// Everything after the time argument, without the quotes the client may have typed.
std::string ExtractReason(const CommandArgs& args) {
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
		const std::string text = FormatText("[Admin System] Usage: %s%s", command, usage);
		Reply(admin, true, text.c_str());
	} else {
		PrintChat(admin, g_adminPlugin.Phrase(phraseKey), command);
	}
}

// The verified target of a *deferred* reply. The offline punishment insert and the group insert
// complete on the database pump, i.e. in a later frame: by then the caller's slot may belong to
// another player. The reply therefore carries the OwnerIdentity captured when the command ran and
// is dropped when OwnerMatches() fails (the server console, slot -1, is always valid).
struct ReplyTarget {
	int32_t slot = kConsoleCaller;
	bool console = true;
	OwnerIdentity owner;

	bool IsClientCaller() const { return !console && slot >= 0; }
};

ReplyTarget MakeReplyTarget(int32_t admin, bool console) {
	ReplyTarget target;
	target.slot = admin;
	target.console = console;
	if (target.IsClientCaller() && !CaptureOwner(admin, target.owner)) {
		target.owner = OwnerIdentity{}; // unverifiable caller: the deferred reply is dropped
	}
	return target;
}

void ReplyDeferred(const ReplyTarget& target, const char* text) {
	if (target.console) {
		Reply(target.slot, true, text); // server console / RCON: no player identity involved
		return;
	}
	if (!target.IsClientCaller() || !OwnerMatches(target.owner)) {
		return; // the slot was re-used (or the player left): never answer the new occupant
	}
	Reply(target.slot, false, text);
}

void PlayerNotFound(int32_t admin, bool console) {
	if (console) {
		Reply(admin, true, "[Admin System] Player not found\n");
	} else {
		PrintChat(admin, "%s", g_adminPlugin.Phrase("PlayerNotFound"));
	}
}

} // namespace

// --- punishment commands (TotalCommand of the original) ------------------------------------

void CommandPunish(int32_t admin, int32_t type, const char* permission, const CommandArgs& args) {
	const bool console = admin == kConsoleCaller;
	if (admin != kConsoleCaller && !HasPermission(admin, permission)) {
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
		// The immunity query and the INSERT are asynchronous: report what actually happened
		// instead of confirming a punishment the core vetoed or failed to store. The reply target
		// is the verified owner of the caller (see ReplyTarget).
		const ReplyTarget replyTarget = MakeReplyTarget(admin, console);
		g_adminPlugin.TryAddOfflinePunishment(
			target, "undefined", type, time, reason, admin, [replyTarget](bool punished) {
				ReplyDeferred(replyTarget, punished ? "[Admin System] Player punished\n"
													: "[Admin System] Player not punished (immunity or database error)\n");
			});
		return;
	}
	PlayerNotFound(admin, console);
}

void CommandUnpunish(int32_t admin, int32_t type, const char* permission, const CommandArgs& args) {
	const bool console = admin == kConsoleCaller;
	if (CoreNotReady(admin, console)) {
		return;
	}
	if (admin != kConsoleCaller && !HasPermission(admin, permission)) {
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

void CommandAddAdmin(int32_t admin, const CommandArgs& args, bool remove, bool console) {
	if (CoreNotReady(admin, console)) {
		return;
	}
	if (admin != kConsoleCaller && !HasPermission(admin, remove ? "@admin/remove" : "@admin/add")) {
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
	// The insert completes on the database pump; the reply carries the verified caller (ReplyTarget).
	const ReplyTarget replyTarget = MakeReplyTarget(admin, console);
	g_adminPlugin.Core().AddGroup(name != nullptr ? name : "", flags != nullptr ? flags : "", immunity,
								  [replyTarget](int64_t groupId) {
									  // GroupAdded of the original (the id comes back from the insert).
									  if (replyTarget.console) {
										  const std::string text =
											  FormatText("[Admin System] Group added (ID: %lld)\n",
														 static_cast<long long>(groupId));
										  Reply(replyTarget.slot, true, text.c_str());
										  return;
									  }
									  if (!replyTarget.IsClientCaller() || !OwnerMatches(replyTarget.owner)) {
										  return; // the slot was re-used: never answer the new occupant
									  }
									  PrintChat(replyTarget.slot, g_adminPlugin.Phrase("GroupAdded"),
												static_cast<int>(groupId));
								  });
}

void CommandAddGroup(int32_t admin, const CommandArgs& args, bool console) {
	if (admin != kConsoleCaller && !HasPermission(admin, "@admin/add_group")) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	if (args.ArgC() < 4) {
		PrintUsage(admin, console, "UsageAddGroup", " <name> <flags> <immunity>\n", args.Arg(0));
		return;
	}
	AddGroupCore(admin, args.Arg(1), args.Arg(2), args.Arg(3) != nullptr ? std::atoi(args.Arg(3)) : 0, console);
}

void CommandRemoveGroup(int32_t admin, const CommandArgs& args, bool console) {
	if (CoreNotReady(admin, console)) {
		return;
	}
	if (admin != kConsoleCaller && !HasPermission(admin, "@admin/remove_group")) {
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
	const bool console = admin == kConsoleCaller;
	if (admin != kConsoleCaller && !HasPermission(admin, "@admin/reload_config")) {
		Reply(admin, console, "[Admin System] You don't have permission to use this command\n");
		return;
	}
	g_adminPlugin.ReloadConfig();
	Reply(admin, console, "[Admin System] Config reloaded\n");
}

void CommandReloadAdmin(int32_t admin, const CommandArgs& args) {
	const bool console = admin == kConsoleCaller;
	if (admin != kConsoleCaller && !HasPermission(admin, "@admin/reload_admin")) {
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
			core->LoadAdmin(slot, value, ClientName(slot));
		}
		return;
	}
	PlayerNotFound(admin, console);
}

void CommandReloadPunish(int32_t admin, const CommandArgs& args) {
	const bool console = admin == kConsoleCaller;
	if (admin != kConsoleCaller && !HasPermission(admin, "@admin/reload_punish")) {
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
			// OnReloadPunish of the original: clear the slot, then force the reload. ResetSlot
			// also drops the admin record (unlike ResetPunishments), so the rights have to be
			// read again too - the reconcile skips this slot once punishmentsLoaded is set.
			core->ResetSlot(slot);
			core->ReloadPunishments(slot, value, ClientIp(slot));
			core->LoadAdmin(slot, value, ClientName(slot));
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
	const char* command;   // the engine name s2sdk dispatches (chat names lose their trigger)
	const char* spelling;  // what the Pisex handler saw as arg[0]
	int32_t type;
	const char* permission;
	bool unpunish;
	bool serverOnly;      // server console only (the original's CON_COMMAND_EXTERN)
	bool chatOnly;        // the "!ban" spelling: dispatched only from chat, like pisexcompat
};

constexpr PunishMapping kPunishMappings[] = {
	{"mm_ban", "mm_ban", RT_BAN, "@admin/ban", false, /*serverOnly=*/true, /*chatOnly=*/false},
	{"mm_mute", "mm_mute", RT_MUTE, "@admin/mute", false, true, false},
	{"mm_gag", "mm_gag", RT_GAG, "@admin/gag", false, true, false},
	{"mm_silence", "mm_silence", RT_SILENCE, "@admin/silence", false, true, false},
	{"mm_unban", "mm_unban", RT_BAN, "@admin/unban", true, true, false},
	{"mm_unmute", "mm_unmute", RT_MUTE, "@admin/unmute", true, true, false},
	{"mm_ungag", "mm_ungag", RT_GAG, "@admin/ungag", true, true, false},
	{"mm_unsilence", "mm_unsilence", RT_SILENCE, "@admin/unsilence", true, true, false},
	{"ban", "!ban", RT_BAN, "@admin/ban", false, false, /*chatOnly=*/true},
	{"mute", "!mute", RT_MUTE, "@admin/mute", false, false, true},
	{"gag", "!gag", RT_GAG, "@admin/gag", false, false, true},
	{"silence", "!silence", RT_SILENCE, "@admin/silence", false, false, true},
	{"unban", "!unban", RT_BAN, "@admin/unban", true, false, true},
	{"unmute", "!unmute", RT_MUTE, "@admin/unmute", true, false, true},
	{"ungag", "!ungag", RT_GAG, "@admin/ungag", true, false, true},
	{"unsilence", "!unsilence", RT_SILENCE, "@admin/unsilence", true, false, true},
};

const PunishMapping* FindPunishMapping(std::string_view engineName) {
	for (const PunishMapping& mapping : kPunishMappings) {
		if (engineName == mapping.command) {
			return &mapping;
		}
	}
	return nullptr;
}

// One command this plugin tried to register. |owned| is s2sdk's AddConsoleCommand result: true
// means the plugin created the engine command (RemoveCommand on unload), false means the name
// already existed and s2sdk only attached our listener (RemoveCommandListener on unload -
// RemoveCommand would touch another plugin's or the engine's command).
struct CommandRegistration {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
	bool owned = false;
};

std::vector<CommandRegistration> g_registrations;

// The Pisex callback contract: the handler gets the raw argument string. The s2sdk callback is
// shared by every command of a family and dispatches on args[0] (the engine's command name).
s2sdk::ResultType OnPunishCommand(int32_t caller, s2sdk::ConCommandContext context,
								  const plg::vector<plg::string>& args) {
	try {
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const std::string_view engineName(args[0].data(), args[0].size());
		const PunishMapping* mapping = FindPunishMapping(engineName);
		if (mapping == nullptr) {
			return s2sdk::ResultType::Continue;
		}
		// The chat spelling is dispatched only from chat (pisexcompat's FindEntry did the same): a
		// "ban" typed into a client console is not the admin command of the original.
		if (mapping->chatOnly && context != s2sdk::ConCommandContext::Chat) {
			return s2sdk::ResultType::Continue;
		}
		// Server-console-only command: exactly -1 runs it, no other caller does (PITFALLS C11:
		// a -2 is not a server console and must never reach a permission check).
		if (mapping->serverOnly) {
			if (caller != kConsoleCaller) {
				return s2sdk::ResultType::Handled;
			}
		} else if (caller != kConsoleCaller && !IsSlotInRange(caller)) {
			// A player command: the server console (-1) or a real slot; a bogus caller is swallowed.
			return s2sdk::ResultType::Handled;
		}
		const std::string content = BuildCommandContent(mapping->spelling, args);
		const CommandArgs line(content.c_str());
		if (mapping->unpunish) {
			CommandUnpunish(caller, mapping->type, mapping->permission, line);
		} else {
			CommandPunish(caller, mapping->type, mapping->permission, line);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in a punishment command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in a punishment command");
	}
	return s2sdk::ResultType::Handled;
}

// mm_admin / css_admin (console) and !admin (chat).
s2sdk::ResultType OnAdminMenuCommand(int32_t caller, s2sdk::ConCommandContext context,
									 const plg::vector<plg::string>& args) {
	try {
		// "mm_admin"/"css_admin" are console names (client-executable, like the original's
		// RegCommand) and "admin" is the chat spelling: each is dispatched only from its own
		// context, exactly like pisexcompat's FindEntry.
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const bool chatSpelling = std::string_view(args[0].data(), args[0].size()) == "admin";
		if (chatSpelling != (context == s2sdk::ConCommandContext::Chat)) {
			return s2sdk::ResultType::Continue;
		}
		// The caller must be the server console (-1) or a real slot: a -2/64 must never reach the
		// menu (PITFALLS C11).
		if (caller != kConsoleCaller && !IsSlotInRange(caller)) {
			return s2sdk::ResultType::Handled;
		}
		g_adminPlugin.ShowAdminMenu(caller);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the admin menu command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the admin menu command");
	}
	return s2sdk::ResultType::Handled;
}

// mm_add_admin / mm_remove_admin / !add_admin / !remove_admin.
s2sdk::ResultType OnAdminManagementCommand(int32_t caller, s2sdk::ConCommandContext context,
										   const plg::vector<plg::string>& args) {
	try {
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const std::string_view engineName(args[0].data(), args[0].size());
		const bool remove = engineName == "mm_remove_admin" || engineName == "remove_admin";
		const bool consoleOnly = engineName == "mm_add_admin" || engineName == "mm_remove_admin";
		const bool console = caller == kConsoleCaller;
		if (!consoleOnly && context != s2sdk::ConCommandContext::Chat) {
			return s2sdk::ResultType::Continue; // the "!add_admin" spelling is chat-only
		}
		if (consoleOnly && caller != kConsoleCaller) {
			return s2sdk::ResultType::Handled; // server console only: exactly -1
		}
		if (!consoleOnly && caller != kConsoleCaller && !IsSlotInRange(caller)) {
			return s2sdk::ResultType::Handled; // bogus caller: no permission check, no reply
		}
		const char* spelling = remove ? (consoleOnly ? "mm_remove_admin" : "!remove_admin")
									  : (consoleOnly ? "mm_add_admin" : "!add_admin");
		const std::string content = BuildCommandContent(spelling, args);
		const CommandArgs line(content.c_str());
		CommandAddAdmin(caller, line, remove, console);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the add/remove admin command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the add/remove admin command");
	}
	return s2sdk::ResultType::Handled;
}

// mm_add_group / mm_remove_group / !add_group / !remove_group.
s2sdk::ResultType OnGroupCommand(int32_t caller, s2sdk::ConCommandContext context,
								 const plg::vector<plg::string>& args) {
	try {
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const std::string_view engineName(args[0].data(), args[0].size());
		const bool remove = engineName == "mm_remove_group" || engineName == "remove_group";
		const bool consoleOnly = engineName == "mm_add_group" || engineName == "mm_remove_group";
		const bool console = caller == kConsoleCaller;
		if (!consoleOnly && context != s2sdk::ConCommandContext::Chat) {
			return s2sdk::ResultType::Continue; // the "!add_group" spelling is chat-only
		}
		if (consoleOnly && caller != kConsoleCaller) {
			return s2sdk::ResultType::Handled; // server console only: exactly -1
		}
		if (!consoleOnly && caller != kConsoleCaller && !IsSlotInRange(caller)) {
			return s2sdk::ResultType::Handled; // bogus caller: no permission check, no reply
		}
		const char* spelling = remove ? (consoleOnly ? "mm_remove_group" : "!remove_group")
									  : (consoleOnly ? "mm_add_group" : "!add_group");
		const std::string content = BuildCommandContent(spelling, args);
		const CommandArgs line(content.c_str());
		if (remove) {
			CommandRemoveGroup(caller, line, console);
		} else {
			CommandAddGroup(caller, line, console);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in the group command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in the group command");
	}
	return s2sdk::ResultType::Handled;
}

// mm_as_reload_config / mm_as_reload_admin / mm_as_reload_punish (server console only).
s2sdk::ResultType OnReloadCommand(int32_t caller, s2sdk::ConCommandContext /*context*/,
								  const plg::vector<plg::string>& args) {
	try {
		if (caller != kConsoleCaller) {
			return s2sdk::ResultType::Handled; // server console only (the original's CON_COMMAND_F)
		}
		if (args.empty()) {
			return s2sdk::ResultType::Continue;
		}
		const std::string_view engineName(args[0].data(), args[0].size());
		const std::string content = BuildCommandContent(engineName, args);
		const CommandArgs line(content.c_str());
		if (engineName == "mm_as_reload_config") {
			CommandReloadConfig(caller);
		} else if (engineName == "mm_as_reload_admin") {
			CommandReloadAdmin(caller, line);
		} else {
			CommandReloadPunish(caller, line);
		}
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in a reload command: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in a reload command");
	}
	return s2sdk::ResultType::Handled;
}

s2sdk::ResultType OnStatusCommand(int32_t caller, s2sdk::ConCommandContext context,
								  const plg::vector<plg::string>& args) {
	(void)args;
	// Server console / RCON only (caller -1 exactly: -2 is not a bypass). The s2sdk chat path
	// executes console commands by name without checking their flags, and the gate the original's
	// status module used for the player list ("@admin/status") is a default_permissions entry for
	// regular players, so "!as_status" would otherwise leak steamids, admin flags, immunity, groups
	// and punishments to any client.
	if (caller != kConsoleCaller) {
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

s2sdk::ConVarFlag CommandFlags(bool serverOnly) {
	// Our own commands: Release always, ClientCanExecute for the ones a player may run (the
	// original registered every "!*" chat name and mm_admin/css_admin as client-executable).
	int64_t bits = static_cast<int64_t>(s2sdk::ConVarFlag::Release);
	if (!serverOnly) {
		bits |= static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute);
	}
	return static_cast<s2sdk::ConVarFlag>(bits);
}

void AddCommandRegistration(const std::string& name, bool serverOnly, s2sdk::ConCommandCallback callback) {
	bool owned = false;
	try {
		owned = s2sdk::AddConsoleCommand(plg::string(name), plg::string(), CommandFlags(serverOnly), callback,
										 s2sdk::HookMode::Pre);
	} catch (...) {
		// The call was interrupted: it cannot be proven that no listener was attached, so the
		// registration is treated as listener-only (the ownership is unknown, and RemoveCommand on
		// a foreign command would break its owner).
		owned = false;
	}
	// s2sdk semantics (3.0.x): AddConsoleCommand returns true only when it created the engine
	// command; false means the name already existed and only our listener was attached.
	g_registrations.push_back(CommandRegistration{name, callback, s2sdk::HookMode::Pre, owned});
}

} // namespace

void RegisterCommands() {
	if (__s2sdk_AddConsoleCommand == nullptr) {
		LogError("admin_system: the s2sdk console command binding is not available - the commands are not "
				 "registered");
		return;
	}
	// Both halves are required: an owned command is detached with RemoveCommand, a pre-existing one
	// (AddConsoleCommand == false) only with RemoveCommandListener. Without them no command may be
	// registered, or its callback would stay attached to the unloaded library (PITFALLS A7).
	if (__s2sdk_RemoveCommand == nullptr || __s2sdk_RemoveCommandListener == nullptr) {
		LogError("admin_system: the s2sdk RemoveCommand/RemoveCommandListener pair is unavailable - the "
				 "commands are not registered");
		return;
	}
	g_registrations.clear();

	// mm_ban / mm_mute / ... / !ban / !mute / ... share one handler.
	for (const PunishMapping& mapping : kPunishMappings) {
		AddCommandRegistration(mapping.command, mapping.serverOnly, &OnPunishCommand);
	}
	// mm_admin / css_admin (console) and !admin (chat): the admin menu.
	for (const char* name : {"mm_admin", "css_admin", "admin"}) {
		AddCommandRegistration(name, false, &OnAdminMenuCommand);
	}
	// Admin management.
	for (const char* name : {"mm_add_admin", "mm_remove_admin"}) {
		AddCommandRegistration(name, true, &OnAdminManagementCommand);
	}
	for (const char* name : {"add_admin", "remove_admin"}) {
		AddCommandRegistration(name, false, &OnAdminManagementCommand);
	}
	// Groups.
	for (const char* name : {"mm_add_group", "mm_remove_group"}) {
		AddCommandRegistration(name, true, &OnGroupCommand);
	}
	for (const char* name : {"add_group", "remove_group"}) {
		AddCommandRegistration(name, false, &OnGroupCommand);
	}
	// The reload commands (server console only).
	for (const char* name : {"mm_as_reload_config", "mm_as_reload_admin", "mm_as_reload_punish"}) {
		AddCommandRegistration(name, true, &OnReloadCommand);
	}
	// as_status (RCON): the status report of this port. Registered without ClientCanExecute; the
	// handler additionally rejects every caller but the server console.
	AddCommandRegistration("as_status", true, &OnStatusCommand);

	LogInfo("admin_system: registered " + std::to_string(g_registrations.size()) + " console/chat commands");
}

void UnregisterCommands() {
	for (const CommandRegistration& registration : g_registrations) {
		try {
			if (registration.owned && __s2sdk_RemoveCommand != nullptr) {
				s2sdk::RemoveCommand(plg::string(registration.name), registration.callback);
			} else if (__s2sdk_RemoveCommandListener != nullptr) {
				// Pre-existing command (or an interrupted registration): only our listener goes.
				s2sdk::RemoveCommandListener(plg::string(registration.name), registration.callback, registration.mode);
			}
		} catch (...) {
		}
	}
	g_registrations.clear();
}

} // namespace admin_system
