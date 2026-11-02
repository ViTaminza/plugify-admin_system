// Tests for the s2sdk guards of plugins/admin_system (src/admin_guards.cpp) with fake bindings.
//
// The bindings are the same `__s2sdk_*` globals the plugin library uses; here they are filled with
// recording fakes, so the guards run exactly as on a live server without an engine:
//
//   * PITFALLS B1 (map gate): the mid-map latch of PollMapGate() opens the gate ONLY on the first
//     update after OnPluginStart, ONLY while no OnMapEnd was seen, ONLY with both map listeners
//     registered, and ONLY when `__s2sdk_GetCurrentMap` exists and reports a real map name
//     ("" and "<empty>" are no proof, and a missing binding is no proof either - then only a real
//     OnMapStart opens the gate). OnMapEnd closes it and only a new OnMapStart re-opens it.
//   * PITFALLS C1: an entity handle is a signed CEntityHandle - 0 and -1 are invalid, a negative
//     handle with a high serial is valid.
//   * PITFALLS C2: HasController() layers the map gate, the slot range, IsClientConnected,
//     IsClientInGame, !IsClientSourceTV, the controller pointer/handle and both validity checks;
//     CaptureOwner()/OwnerMatches() pin the SteamID64 *and* the controller handle.
//   * PITFALLS C11: PrintConsole() prints to the server console only for exactly slot -1; any other
//     out-of-range slot prints nothing at all.
//
// Run by ./test.sh (build/tests/admin_gate_tests).

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include "admin_guards.hpp"

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

// --- the fake s2sdk bindings ---------------------------------------------------------------

using MapCallback = void (*)();

MapCallback g_onMapStart = nullptr;
MapCallback g_onMapEnd = nullptr;
std::string g_mapName = "de_dust2";

void FakeOnMapStartRegister(MapCallback callback) {
	g_onMapStart = callback;
}
void FakeOnMapStartUnregister(MapCallback callback) {
	if (g_onMapStart == callback) {
		g_onMapStart = nullptr;
	}
}
void FakeOnMapEndRegister(MapCallback callback) {
	g_onMapEnd = callback;
}
void FakeOnMapEndUnregister(MapCallback callback) {
	if (g_onMapEnd == callback) {
		g_onMapEnd = nullptr;
	}
}
plg::string FakeGetCurrentMap() {
	return plg::string(g_mapName);
}

// One fake client in slot 3.
bool g_connected = true;
bool g_inGame = true;
bool g_sourceTV = false;
bool g_fakeClient = false;
bool g_authorized = true;
uint64_t g_steamid = 76561198000000001ull;
std::string g_clientName = "Admin";
int g_entity = 0;
int32_t g_controllerHandle = 0x12345;
bool g_validHandle = true;
bool g_validPointer = true;

bool FakeIsClientConnected(int32_t slot) {
	return slot == 3 && g_connected;
}
bool FakeIsClientInGame(int32_t slot) {
	return slot == 3 && g_inGame;
}
bool FakeIsClientSourceTV(int32_t slot) {
	return slot == 3 && g_sourceTV;
}
bool FakeIsFakeClient(int32_t slot) {
	return slot == 3 && g_fakeClient;
}
bool FakeIsClientAuthorized(int32_t slot) {
	return slot == 3 && g_authorized;
}
uint64_t FakeGetClientSteamID64(int32_t slot) {
	return slot == 3 ? g_steamid : 0;
}
plg::string FakeGetClientName(int32_t slot) {
	return slot == 3 ? plg::string(g_clientName) : plg::string();
}
plg::string FakeGetClientIp(int32_t slot) {
	return slot == 3 ? plg::string("1.2.3.4") : plg::string();
}
void* FakePlayerSlotToEntPointer(int32_t slot) {
	return slot == 3 ? &g_entity : nullptr;
}
int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	return slot == 3 ? g_controllerHandle : 0;
}
bool FakeIsValidEntHandle(int32_t handle) {
	return g_validHandle && handle != 0 && handle != -1;
}
bool FakeIsValidEntPointer(void* pointer) {
	return g_validPointer && pointer != nullptr;
}

// Output recorders.
std::vector<std::string> g_serverConsole;
std::vector<std::string> g_clientConsole;
std::vector<std::string> g_chat;

void FakeLog(std::string_view message, plg::Severity, const plg::source_location&) {
	g_serverConsole.emplace_back(message);
}
void FakePrintToConsole(int32_t, const plg::string& message) {
	g_clientConsole.emplace_back(std::string_view(message));
}
void FakePrintToChat(int32_t, const plg::string& message) {
	g_chat.emplace_back(std::string_view(message));
}
void FakePrintToChatAll(const plg::string& message) {
	g_chat.emplace_back(std::string_view(message));
}

void InstallBindings() {
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

	__s2sdk_PrintToConsole = &FakePrintToConsole;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_PrintToChatAll = &FakePrintToChatAll;
}

void ResetGate() {
	admin_system::ShutdownMapGate();
	admin_system::SetMapLifecycleCallbacks(nullptr, nullptr);
	g_onMapStart = nullptr;
	g_onMapEnd = nullptr;
}

} // namespace

// The plugin's log function is defined by PLUGIFY_PLUGIN in the shared library; this test does not
// link plugin.cpp, so it provides the recorder itself.
namespace plg {
LogFn Log = &FakeLog;
} // namespace plg

int main() {
	using namespace admin_system;

	InstallBindings();

	// --- B1: a missing GetCurrentMap binding is no proof of a live map --------------------------
	ResetGate();
	__s2sdk_GetCurrentMap = nullptr;
	InitMapGate();
	CHECK(g_onMapStart != nullptr && g_onMapEnd != nullptr); // both listeners registered
	PollMapGate();
	CHECK(!IsMapReady()); // the latch cannot prove a map: the gate stays shut
	// Only a real OnMapStart opens it then.
	g_onMapStart();
	CHECK(IsMapReady());
	ResetGate();

	// --- B1: "" and "<empty>" are no proof either ----------------------------------------------
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	g_mapName.clear();
	InitMapGate();
	PollMapGate();
	CHECK(!IsMapReady());
	ResetGate();

	g_mapName = "<empty>";
	InitMapGate();
	PollMapGate();
	CHECK(!IsMapReady());
	ResetGate();

	// --- B1: a real map name opens the gate on the first update --------------------------------
	g_mapName = "de_dust2";
	InitMapGate();
	PollMapGate();
	CHECK(IsMapReady());

	// OnMapEnd closes it, and the latch does NOT re-open it (only a new OnMapStart does).
	g_onMapEnd();
	CHECK(!IsMapReady());
	PollMapGate();
	CHECK(!IsMapReady());
	g_onMapStart();
	CHECK(IsMapReady());
	ResetGate();

	// --- B1/A7: a partial listener pair registers nothing and the latch stays shut --------------
	__s2sdk_GetCurrentMap = &FakeGetCurrentMap;
	g_mapName = "de_dust2";
	__s2sdk_OnMapEnd_Register = nullptr;
	__s2sdk_OnMapEnd_Unregister = nullptr;
	InitMapGate();
	CHECK(g_onMapStart == nullptr); // nothing was registered (A7: no half pairs)
	PollMapGate();
	CHECK(!IsMapReady());
	InstallBindings(); // restore
	ResetGate();

	// --- an open gate for the client/entity/printer checks below -------------------------------
	g_mapName = "de_dust2";
	InitMapGate();
	g_onMapStart();
	CHECK(IsMapReady());

	// --- C1: signed entity handles -------------------------------------------------------------
	CHECK(!IsUsableEntityHandle(0));
	CHECK(!IsUsableEntityHandle(-1));
	CHECK(IsUsableEntityHandle(5));
	CHECK(IsUsableEntityHandle(-12345)); // a valid negative handle (high serial bit) is usable
	CHECK(IsUsableEntityHandle(g_controllerHandle));
	g_validHandle = false;
	CHECK(!IsUsableEntityHandle(5));
	g_validHandle = true;

	// --- C2: the slot/controller chain ---------------------------------------------------------
	CHECK(HasClient(3));
	CHECK(HasController(3));
	CHECK(!HasController(4));
	g_sourceTV = true;
	CHECK(!HasController(3)); // the SourceTV proxy is not a player
	g_sourceTV = false;
	g_inGame = false;
	CHECK(HasClient(3));      // IsClientConnected still true ...
	CHECK(!HasController(3)); // ... but a client that is not in game has no usable controller
	g_inGame = true;
	g_validPointer = false;
	CHECK(!HasController(3));
	g_validPointer = true;
	CHECK(IsSlotInRange(0) && IsSlotInRange(63) && !IsSlotInRange(-1) && !IsSlotInRange(64));

	// ClientName drops s2sdk's "<unknown>" placeholder and answers "" without a controller.
	CHECK(ClientName(3) == "Admin");
	g_clientName = "<unknown>";
	CHECK(ClientName(3).empty());
	g_clientName = "Admin";
	CHECK(ClientName(4).empty());
	CHECK(ClientSteamID64(4) == 0);
	CHECK(ClientIp(3) == "1.2.3.4");
	CHECK(ClientIp(4).empty());

	// --- owner identity: SteamID64 *and* controller handle --------------------------------------
	OwnerIdentity owner;
	CHECK(CaptureOwner(3, owner));
	CHECK(owner.slot == 3);
	CHECK(owner.steamId == g_steamid);
	CHECK(owner.controllerHandle == g_controllerHandle);
	CHECK(OwnerMatches(owner));
	g_steamid = 76561198000000002ull; // the slot was re-used by another player
	CHECK(!OwnerMatches(owner));
	g_steamid = owner.steamId;
	g_controllerHandle = 0x999; // same account, new controller generation
	CHECK(!OwnerMatches(owner));
	g_controllerHandle = owner.controllerHandle;
	CHECK(OwnerMatches(owner));
	CHECK(!OwnerMatches(OwnerIdentity{}));
	CHECK(!CaptureOwner(4, owner));
	CHECK(!owner.IsSet());
	g_controllerHandle = -1; // INVALID_EHANDLE_INDEX is never a usable controller handle
	CHECK(!CaptureOwner(3, owner));
	g_controllerHandle = 0x12345;

	// --- C11: PrintConsole prints to the server console only for exactly slot -1 -----------------
	g_serverConsole.clear();
	g_clientConsole.clear();
	PrintConsole(-1, "%s", "server console line");
	CHECK(g_serverConsole.size() == 1);
	CHECK(!g_serverConsole.empty() && g_serverConsole.back() == "server console line");
	PrintConsole(-2, "%s", "not a console");   // -2 is NOT the server console
	PrintConsole(64, "%s", "not a slot");
	PrintConsole(-100, "%s", "not a console");
	CHECK(g_serverConsole.size() == 1); // nothing was logged
	CHECK(g_clientConsole.empty());
	PrintConsole(3, "%s", "client console line"); // a real slot goes to the client console
	CHECK(g_clientConsole.size() == 1);
	CHECK(!g_clientConsole.empty() && g_clientConsole.back() == "client console line");
	// The colour tags are stripped for the server console and expanded for chat.
	PrintConsole(-1, "%s", "{RED}red{DEFAULT}");
	CHECK(g_serverConsole.back() == "red");

	// --- B2: with the gate closed no client/printer call happens --------------------------------
	ResetGate();
	CHECK(!IsMapReady());
	g_clientConsole.clear();
	g_serverConsole.clear();
	PrintConsole(3, "%s", "no map");
	PrintConsole(-1, "%s", "no map console");
	CHECK(g_clientConsole.empty());
	CHECK(g_serverConsole.size() == 1); // the server console is not a client call
	CHECK(HasClient(3) == false);
	CHECK(!HasController(3));
	CHECK(ClientName(3).empty());
	CHECK(ClientSteamID64(3) == 0);
	CHECK(!CaptureOwner(3, owner));

	if (g_failures != 0) {
		std::printf("admin_gate_tests: %d/%d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("admin_gate_tests: all %d checks passed\n", g_checks);
	return 0;
}
