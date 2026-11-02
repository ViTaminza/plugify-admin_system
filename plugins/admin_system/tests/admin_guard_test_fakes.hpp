// Recording s2sdk fakes for the admin_system runtime tests (tests only).
//
// The plugin library reads the generated binding globals (`__s2sdk_*`); the tests fill them with
// these fakes instead of the module, so the real plugin translation units run without an engine:
// one fake client in slot 3, a controllable map, a command table that records every registration,
// and output recorders for chat / client console / server console.
//
// Header-only and included by exactly one test translation unit, which links `s2sdk_bindings` for
// the globals themselves.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

namespace admin_test {

// --- recorded output -------------------------------------------------------------------------

inline std::vector<std::pair<int32_t, std::string>> g_chat;     // PrintToChat / PrintToChatAll
inline std::vector<std::string> g_clientConsole;                // PrintToConsole(slot >= 0)
inline std::vector<std::string> g_serverConsole;                // plg::Log (PrintConsole(-1), errors)

inline void ClearOutput() {
	g_chat.clear();
	g_clientConsole.clear();
	g_serverConsole.clear();
}

inline const std::vector<std::pair<int32_t, std::string>>& ChatOutput() {
	return g_chat;
}
inline const std::pair<int32_t, std::string>& LastChat() {
	return g_chat.back();
}
inline const std::vector<std::string>& ServerConsoleOutput() {
	return g_serverConsole;
}
inline const std::string& LastServerConsole() {
	return g_serverConsole.back();
}

inline void FakeLog(std::string_view message, plg::Severity, const plg::source_location&) {
	g_serverConsole.emplace_back(message);
}
inline void FakePrintToChat(int32_t slot, const plg::string& message) {
	g_chat.emplace_back(slot, std::string(std::string_view(message)));
}
inline void FakePrintToChatAll(const plg::string& message) {
	g_chat.emplace_back(-1, std::string(std::string_view(message)));
}
inline void FakePrintToConsole(int32_t, const plg::string& message) {
	g_clientConsole.emplace_back(std::string_view(message));
}

// --- the fake map ----------------------------------------------------------------------------

using MapCallback = void (*)();

inline MapCallback g_onMapStart = nullptr;
inline MapCallback g_onMapEnd = nullptr;
inline std::string g_mapName = "de_dust2";

inline void FakeOnMapStartRegister(MapCallback callback) {
	g_onMapStart = callback;
}
inline void FakeOnMapStartUnregister(MapCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
}
inline void FakeOnMapEndRegister(MapCallback callback) {
	g_onMapEnd = callback;
}
inline void FakeOnMapEndUnregister(MapCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
}
inline plg::string FakeGetCurrentMap() {
	return plg::string(g_mapName);
}

// --- the fake client (slot 3) ----------------------------------------------------------------

inline bool g_connected = true;
inline bool g_inGame = true;
inline bool g_sourceTV = false;
inline bool g_fakeClient = false;
inline bool g_authorized = true;
inline uint64_t g_steamid = 76561198000000001ull;
inline std::string g_clientName = "Admin";
inline std::string g_clientIp = "1.2.3.4";
inline int g_entity = 0;
inline int32_t g_controllerHandle = 0x12345;

inline bool FakeIsClientConnected(int32_t slot) {
	return slot == 3 && g_connected;
}
inline bool FakeIsClientInGame(int32_t slot) {
	return slot == 3 && g_inGame;
}
inline bool FakeIsClientSourceTV(int32_t slot) {
	return slot == 3 && g_sourceTV;
}
inline bool FakeIsFakeClient(int32_t slot) {
	return slot == 3 && g_fakeClient;
}
inline bool FakeIsClientAuthorized(int32_t slot) {
	return slot == 3 && g_authorized;
}
inline uint64_t FakeGetClientSteamID64(int32_t slot) {
	return slot == 3 ? g_steamid : 0;
}
inline plg::string FakeGetClientName(int32_t slot) {
	return slot == 3 ? plg::string(g_clientName) : plg::string();
}
inline plg::string FakeGetClientIp(int32_t slot) {
	return slot == 3 ? plg::string(g_clientIp) : plg::string();
}
inline void* FakePlayerSlotToEntPointer(int32_t slot) {
	return slot == 3 ? &g_entity : nullptr;
}
inline int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	return slot == 3 ? g_controllerHandle : 0;
}
inline bool FakeIsValidEntHandle(int32_t handle) {
	return handle != 0 && handle != -1;
}
inline bool FakeIsValidEntPointer(void* pointer) {
	return pointer != nullptr;
}

inline void SetClientSteamId(uint64_t steamid) {
	g_steamid = steamid;
}

// --- the fake command table ------------------------------------------------------------------

struct CommandRecord {
	std::string name;
	s2sdk::ConCommandCallback callback = nullptr;
	bool owned = false;
};

inline std::vector<CommandRecord> g_commands;
inline std::vector<std::string> g_sayListeners;

inline bool FakeAddConsoleCommand(const plg::string& name, const plg::string&, s2sdk::ConVarFlag,
								  s2sdk::ConCommandCallback callback, s2sdk::HookMode) {
	g_commands.push_back(CommandRecord{std::string(std::string_view(name)), callback, /*owned=*/true});
	return true; // this plugin created the engine command
}
inline bool FakeRemoveCommand(const plg::string&, s2sdk::ConCommandCallback) {
	return true;
}
inline bool FakeAddCommandListener(const plg::string& name, s2sdk::ConCommandCallback, s2sdk::HookMode) {
	g_sayListeners.emplace_back(std::string_view(name));
	return true;
}
inline bool FakeRemoveCommandListener(const plg::string&, s2sdk::ConCommandCallback, s2sdk::HookMode) {
	return true;
}

// The callback registered for an engine name (the most recent registration wins), nullptr when the
// command was never registered.
inline s2sdk::ConCommandCallback FindCommand(std::string_view name) {
	for (auto it = g_commands.rbegin(); it != g_commands.rend(); ++it) {
		if (it->name == name) {
			return it->callback;
		}
	}
	return nullptr;
}

// --- installation ----------------------------------------------------------------------------

inline void InstallFakeBindings() {
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;

	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsFakeClient = &FakeIsFakeClient;
	__s2sdk_IsClientAuthorized = &FakeIsClientAuthorized;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientIp = &FakeGetClientIp;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;

	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintToChatAll = &FakePrintToChatAll;
	__s2sdk_PrintToConsole = &FakePrintToConsole;

	// The command layer requires the whole Add/Remove set before it registers anything (A7).
	__s2sdk_AddConsoleCommand = &FakeAddConsoleCommand;
	__s2sdk_RemoveCommand = &FakeRemoveCommand;
	__s2sdk_AddCommandListener = &FakeAddCommandListener;
	__s2sdk_RemoveCommandListener = &FakeRemoveCommandListener;
}

} // namespace admin_test
