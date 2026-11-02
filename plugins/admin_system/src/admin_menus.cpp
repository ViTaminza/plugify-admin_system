// admin_system — the native s2sdk menu layer (see admin_menus.hpp for the behaviour and the
// safety rules it preserves).

#include "admin_menus.hpp"

#include <hudmenu/hudmenu.hpp>
#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace admin_system {
namespace {

using MenuCallback = std::function<void(const char*, const char*, int, int)>;

struct OpenMenu {
	s2sdk::MenuId id = 0;
	OwnerIdentity owner;
	MenuCallback callback;
	std::vector<MenuEntry> items; // index-aligned with the s2sdk item list
};

struct PendingAction {
	enum class Kind {
		Select,
		ExitBack,
		Exit,
	};
	Kind kind = Kind::Select;
	OwnerIdentity owner;
	MenuCallback callback;
	std::string back;
	std::string front;
	int item = 0;
};

struct PendingShow {
	OwnerIdentity owner;
	Menu menu;
};

struct PendingClose {
	OwnerIdentity owner;
};

std::array<OpenMenu, kMaxSlots> g_open{};
std::vector<PendingAction> g_actions;
std::vector<PendingShow> g_shows;
std::vector<PendingClose> g_closes;
std::vector<s2sdk::MenuId> g_destroy;

// True while a menu handler is running: the engine calls handlers with the menu manager's
// recursive mutex held, so nothing may be created, displayed or destroyed from here.
bool g_inHandler = false;
// This plugin created the reserved s2sdk menu id 0 (see StartMenuLayer) and must destroy it before
// unloading, or its handler points into unloaded code after `plg reload`.
bool g_reservedIdZero = false;
// One error line per library instance when the lifecycle bindings are incomplete.
bool g_bindingsErrorLogged = false;

void* g_user = nullptr;
MenuOwnerEligibleHandler g_eligible = nullptr;

void ReportMissingBindings() {
	if (g_bindingsErrorLogged) {
		return;
	}
	g_bindingsErrorLogged = true;
	LogError("admin_system: the s2sdk menu bindings are incomplete (create/add/display/get/cancel/"
			 "destroy, the display setters, set/get menu type or GetClientSteamID64 are missing) - "
			 "the admin menu is disabled");
}

// The "hud" backend (plugins/hudmenu) is cs2-menus' HUD_LAYOUT: a Panorama on-screen menu driven by
// mouse clicks, usable while dead.
bool IsTypeRegistered(std::string_view name) {
	if (__s2sdk_IsMenuTypeRegistered == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsMenuTypeRegistered(plg::string(name));
	} catch (...) {
		return false;
	}
}

// The player's own "!menu" choice, asked from the hudmenu plugin. Empty when hudmenu is not
// installed, the slot has no live client or anything went wrong - ResolveMenuTypeName then falls
// back to the HUD_LAYOUT mapping.
std::string PlayerTypeChoice(int32_t slot) {
	if (__hudmenu_GetClientMenuType == nullptr || !IsMapReady()) {
		return {};
	}
	try {
		const plg::string answer = hudmenu::GetClientMenuType(slot);
		return std::string(std::string_view(answer));
	} catch (...) {
		return {};
	}
}

void ClearSlot(int32_t slot) {
	g_open[static_cast<std::size_t>(slot)] = OpenMenu{};
}

bool IsTrackedMenu(s2sdk::MenuId id) {
	for (const OpenMenu& state : g_open) {
		if (state.id == id) {
			return true;
		}
	}
	return false;
}

// The owner is still the verified player of the request AND (when the plugin installed the hook) the
// slot's admin data is authoritative for exactly this steamid.
bool OwnerEligible(const OwnerIdentity& owner) {
	if (!OwnerMatches(owner)) {
		return false;
	}
	if (g_eligible == nullptr) {
		return true;
	}
	try {
		return g_eligible(g_user, owner);
	} catch (...) {
		return false;
	}
}

void QueueMenuDestroy(s2sdk::MenuId id) {
	if (id == 0) {
		return; // s2sdk's "no menu"
	}
	for (const s2sdk::MenuId queued : g_destroy) {
		if (queued == id) {
			return; // Cancel and End both report the same session end
		}
	}
	g_destroy.push_back(id);
}

// Ends every display of `id` with MenuCancelReason::Disconnect (s2sdk skips the menu backend for
// that reason, so no client call happens). Returns true only when no client state references the
// id any more - the precondition for a safe DestroyMenu(). A failed/refused cancel returns false.
bool EnsureNotDisplayed(s2sdk::MenuId id) {
	if (__s2sdk_GetClientMenu == nullptr || __s2sdk_CancelClientMenu == nullptr) {
		return false;
	}
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		s2sdk::MenuId current = 0;
		try {
			current = s2sdk::GetClientMenu(slot);
		} catch (...) {
			return false;
		}
		if (current != id) {
			continue;
		}
		try {
			s2sdk::CancelClientMenu(slot, s2sdk::MenuCancelReason::Disconnect);
		} catch (...) {
		}
		// Confirm the session is really gone before DestroyMenu may close the backend: this covers
		// both a refused cancel and an engine that reports success without clearing the state.
		try {
			if (s2sdk::GetClientMenu(slot) == id) {
				return false;
			}
		} catch (...) {
			return false;
		}
	}
	return true;
}

// The last-resort map-down teardown. A displayed session that refuses CancelClientMenu(Disconnect)
// can still be removed safely: switch the menu to one of the built-in digit backends whose close
// callback is a no-op (ChatMenu_Close / ConsoleMenu_Close in s2sdk's menu_types_builtin.cpp),
// verify the switch with GetMenuType, then DestroyMenu. DestroyMenu's Destroyed path then calls the
// no-op close instead of the custom hudmenu/wasd backend, so no client call happens and the menu
// (and its handler pointer) leaves the manager before the library unloads.
bool ForceSafeDestroy(s2sdk::MenuId id) {
	if (__s2sdk_SetMenuType == nullptr || __s2sdk_GetMenuType == nullptr ||
		__s2sdk_DestroyMenu == nullptr) {
		return false;
	}
	constexpr const char* kSafeTypes[] = {"chat", "console"};
	for (const char* type : kSafeTypes) {
		bool switched = false;
		try {
			switched = s2sdk::SetMenuType(id, plg::string(type));
		} catch (...) {
			switched = false;
		}
		if (!switched) {
			// The menu no longer exists: there is no callback left to unload.
			try {
				if (s2sdk::GetMenuType(id).empty()) {
					return true;
				}
			} catch (...) {
			}
			continue;
		}
		plg::string actual;
		try {
			actual = s2sdk::GetMenuType(id);
		} catch (...) {
			continue;
		}
		if (std::string_view(actual) != type) {
			continue;
		}
		try {
			s2sdk::DestroyMenu(id); // Destroyed -> the builtin no-op close, no client call
			return true;
		} catch (...) {
			return false;
		}
	}
	return false;
}

// Runs outside the menu manager's lock. `allowBackendClose` is true only while the map is up (the
// unload-mid-map path), where DestroyMenu may safely close the backend display. With the gate
// closed the id goes through the confirmed-cancel path first and the builtin-type escape second.
void DestroyQueuedMenus(bool allowBackendClose) {
	if (g_destroy.empty()) {
		return;
	}
	std::vector<s2sdk::MenuId> ids = std::move(g_destroy);
	g_destroy.clear();
	for (const s2sdk::MenuId id : ids) {
		if (IsTrackedMenu(id)) {
			g_destroy.push_back(id); // a current display: ApplyShow/Shutdown owns it
			continue;
		}
		if (__s2sdk_DestroyMenu == nullptr) {
			g_destroy.push_back(id);
			continue;
		}
		const bool safe = id == 0 || EnsureNotDisplayed(id);
		if (!safe) {
			if (allowBackendClose && IsMapReady()) {
				try {
					s2sdk::DestroyMenu(id); // map is up: the backend close is safe
				} catch (...) {
				}
			} else if (!ForceSafeDestroy(id)) {
				g_destroy.push_back(id); // retried on the next update / at map start
			}
			continue;
		}
		try {
			s2sdk::DestroyMenu(id);
		} catch (...) {
			g_destroy.push_back(id);
		}
	}
}

// Must be called outside a handler; while the map is up DestroyMenu also closes the backend display.
// Clears the slot first: DestroyMenu re-enters the menu manager and calls the handler's Cancel/End,
// which then sees a stale id and does nothing.
void DestroySlotMenu(int32_t slot) {
	const OpenMenu state = g_open[static_cast<std::size_t>(slot)];
	ClearSlot(slot);
	if (state.id != 0 && __s2sdk_DestroyMenu != nullptr) {
		try {
			s2sdk::DestroyMenu(state.id);
		} catch (...) {
		}
	}
}

void MenuHandler(s2sdk::MenuId id, s2sdk::MenuAction action, int32_t slot, int32_t param) {
	// Everything the handler asks for goes through the queues. The flag is saved/restored rather
	// than cleared, so a re-entering handler does not clear it for the outer one.
	struct HandlerScope {
		bool previous;
		HandlerScope() : previous(g_inHandler) { g_inHandler = true; }
		~HandlerScope() { g_inHandler = previous; }
	} scope;
	try {
		if (!IsSlotInRange(slot)) {
			return;
		}
		OpenMenu& state = g_open[static_cast<std::size_t>(slot)];
		if (state.id != id) {
			return; // stale: the plugin already closed/replaced it
		}
		switch (action) {
		case s2sdk::MenuAction::Select: {
			if (param < 0 || static_cast<std::size_t>(param) >= state.items.size()) {
				return;
			}
			// s2sdk accepts digits 1..7 even when the page shows five items and maps them to
			// currentOffset + digit - 1, so 6/7 select an item of the next page that was never
			// displayed. Pisex numbered the five visible items 1..5.
			int32_t offset = param - param % kItemsPerPage;
			if (__s2sdk_GetClientMenuOffset != nullptr) {
				try {
					offset = s2sdk::GetClientMenuOffset(slot); // read-only, recursive mutex
				} catch (...) {
				}
			}
			const int32_t button = VisibleMenuButton(param, offset, kItemsPerPage);
			if (button == 0) {
				return; // digit 6/7 (next page) or a stale index of another page
			}
			const MenuEntry item = state.items[static_cast<std::size_t>(param)];
			if (item.disabled) {
				return;
			}
			PendingAction pending;
			pending.kind = PendingAction::Kind::Select;
			pending.owner = state.owner;
			pending.callback = state.callback;
			pending.back = item.back;
			pending.front = item.text;
			pending.item = button;
			g_actions.push_back(std::move(pending));
			break;
		}
		case s2sdk::MenuAction::Cancel: {
			const s2sdk::MenuId menuId = state.id;
			const OwnerIdentity owner = state.owner;
			const MenuCallback callback = state.callback;
			ClearSlot(slot);
			QueueMenuDestroy(menuId);
			// Pisex's exit/back values; a timeout or a disconnect just drops the menu.
			const auto reason = static_cast<s2sdk::MenuCancelReason>(param);
			if (reason == s2sdk::MenuCancelReason::Exit || reason == s2sdk::MenuCancelReason::ExitBack) {
				PendingAction pending;
				pending.kind = reason == s2sdk::MenuCancelReason::Exit ? PendingAction::Kind::Exit
																	   : PendingAction::Kind::ExitBack;
				pending.owner = owner;
				pending.callback = callback;
				pending.back = pending.kind == PendingAction::Kind::Exit ? "exit" : "back";
				pending.front = pending.back;
				pending.item = pending.kind == PendingAction::Kind::Exit ? 9 : 7;
				g_actions.push_back(std::move(pending));
			}
			break;
		}
		case s2sdk::MenuAction::End: {
			const s2sdk::MenuId menuId = state.id;
			ClearSlot(slot);
			QueueMenuDestroy(menuId);
			break;
		}
		case s2sdk::MenuAction::Start:
			break;
		}
	} catch (...) {
		// Never unwind into the engine; the manager's lock must be released cleanly.
	}
}

void ApplyAction(const PendingAction& action) {
	if (!IsMapReady() || !OwnerEligible(action.owner)) {
		return; // stale owner / admin data not authoritative: never apply to the current occupant
	}
	if (!action.callback) {
		return;
	}
	try {
		action.callback(action.back.c_str(), action.front.c_str(), action.item, action.owner.slot);
	} catch (const std::exception& ex) {
		LogError(std::string("admin_system: exception in a menu callback: ") + ex.what());
	} catch (...) {
		LogError("admin_system: unknown exception in a menu callback");
	}
}

void ApplyShow(PendingShow& show) {
	if (!IsMapReady()) {
		return;
	}
	if (!MenuBindingsAvailable()) {
		ReportMissingBindings();
		return;
	}
	if (!OwnerEligible(show.owner)) {
		return; // the request's player left / the slot was re-used / data not loaded: drop it
	}
	DestroySlotMenu(show.owner.slot); // replace whatever this client had open

	s2sdk::MenuId id = 0;
	try {
		id = s2sdk::CreateMenu(plg::string(show.menu.title), &MenuHandler,
							   plg::string(ResolveMenuTypeName(show.owner.slot)));
	} catch (...) {
		id = 0;
	}
	if (id == 0) {
		// Only possible when the id-0 reservation did not happen: the menu would be untrackable
		// (0 is "no menu"). Destroy it while the map is up.
		if (__s2sdk_DestroyMenu != nullptr) {
			try {
				s2sdk::DestroyMenu(id);
			} catch (...) {
			}
		}
		LogError("admin_system: s2sdk returned MenuId 0 - the admin menu was not displayed");
		return;
	}

	OpenMenu& state = g_open[static_cast<std::size_t>(show.owner.slot)];
	state.id = id;
	state.owner = show.owner;
	state.callback = show.menu.callback;
	for (const MenuEntry& item : show.menu.items) {
		int32_t index = -1;
		try {
			index = s2sdk::AddMenuItem(id, plg::string(item.back), plg::string(item.text),
									   item.disabled ? s2sdk::MenuItemStyle::Disabled
													 : s2sdk::MenuItemStyle::Default);
		} catch (...) {
			index = -1;
		}
		if (index >= 0) {
			state.items.push_back(item); // index-aligned with the s2sdk item list
		}
	}
	// The complete binding set is a precondition (MenuBindingsAvailable above), so these cannot be
	// null calls; a throwing export still leaves a tracked menu that ShutdownMenuLayer destroys.
	try {
		s2sdk::SetMenuPagination(id, kItemsPerPage);
		s2sdk::SetMenuExitButton(id, show.menu.exitButton);
		s2sdk::SetMenuExitBackButton(id, show.menu.backButton);
		// The original kept the menu open after a selection and decided itself what happened next.
		s2sdk::SetMenuCloseOnSelect(id, false);
	} catch (...) {
	}

	bool displayed = false;
	try {
		displayed = s2sdk::DisplayMenu(id, show.owner.slot, kMenuTimeoutSeconds);
	} catch (...) {
		displayed = false;
	}
	if (!displayed) {
		LogError("admin_system: s2sdk could not display the admin menu");
		DestroySlotMenu(show.owner.slot);
	}
}

void ReleaseReservedMenuId() {
	if (!g_reservedIdZero) {
		return;
	}
	g_reservedIdZero = false;
	if (__s2sdk_DestroyMenu == nullptr) {
		return;
	}
	try {
		s2sdk::DestroyMenu(0); // never displayed (0 is "no menu"), so no backend close
	} catch (...) {
	}
}

// The map-end / unload-while-map-down path: no client call may happen. Every tracked menu is queued
// and destroyed only after CancelClientMenu(Disconnect) has been confirmed; a refused cancel uses the
// builtin no-op-close type escape (ForceSafeDestroy) or leaves the id queued for a later safe
// attempt instead of forcing a backend close.
void CloseAllNoMap() {
	g_actions.clear();
	g_shows.clear();
	g_closes.clear();
	for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
		const s2sdk::MenuId id = g_open[static_cast<std::size_t>(slot)].id;
		if (id == 0) {
			continue;
		}
		ClearSlot(slot);
		QueueMenuDestroy(id);
	}
	DestroyQueuedMenus(false);
}

} // namespace

// --- builder (Pisex's menu API) ----------------------------------------------------------------

void SetMenuTitle(Menu& menu, std::string title) {
	menu.title = std::move(title);
}

void AddMenuItem(Menu& menu, std::string back, std::string text, bool disabled) {
	MenuEntry item;
	item.back = std::move(back);
	item.text = std::move(text);
	item.disabled = disabled;
	menu.items.push_back(std::move(item));
}

void SetMenuExitButton(Menu& menu, bool enabled) {
	menu.exitButton = enabled;
}

void SetMenuExitBackButton(Menu& menu, bool enabled) {
	menu.backButton = enabled;
}

void SetMenuCallback(Menu& menu, std::function<void(const char*, const char*, int, int)> callback) {
	menu.callback = std::move(callback);
}

void DisplayMenu(Menu menu, int32_t slot) {
	if (!IsSlotInRange(slot) || !IsMapReady()) {
		return; // no map: no client to show it to
	}
	if (!MenuBindingsAvailable()) {
		ReportMissingBindings();
		return;
	}
	OwnerIdentity owner;
	if (!CaptureOwner(slot, owner)) {
		return; // unverified owner: never show the admin menu
	}
	// Reuse the slot's queued show instead of piling up replacements (a fast second selection).
	for (PendingShow& queued : g_shows) {
		if (queued.owner.slot == slot) {
			queued.owner = owner;
			queued.menu = std::move(menu);
			return;
		}
	}
	g_shows.push_back(PendingShow{owner, std::move(menu)});
}

void CloseMenu(int32_t slot) {
	if (!IsSlotInRange(slot) || !IsMapReady()) {
		return;
	}
	OwnerIdentity owner;
	if (!CaptureOwner(slot, owner)) {
		return;
	}
	for (PendingClose& queued : g_closes) {
		if (queued.owner.slot == slot) {
			queued.owner = owner;
			return;
		}
	}
	g_closes.push_back(PendingClose{owner});
}

// --- lifecycle ---------------------------------------------------------------------------------

bool MenuBindingsAvailable() {
	// The whole lifecycle and the owner identity, before ANY menu is registered/created:
	//   create/add/display  - the menu itself,
	//   get/cancel/destroy  - the only safe teardown path (s2sdk never destroys a menu itself),
	//   set/get menu type   - the map-down escape to a builtin backend with a no-op close,
	//   the display setters - the Pisex pagination/exit/back/close-on-select behaviour,
	//   GetClientSteamID64  - the owner identity every queued request is re-validated against.
	return __s2sdk_CreateMenu != nullptr && __s2sdk_AddMenuItem != nullptr &&
		   __s2sdk_DisplayMenu != nullptr && __s2sdk_GetClientMenu != nullptr &&
		   __s2sdk_CancelClientMenu != nullptr && __s2sdk_DestroyMenu != nullptr &&
		   __s2sdk_SetMenuPagination != nullptr && __s2sdk_SetMenuExitButton != nullptr &&
		   __s2sdk_SetMenuExitBackButton != nullptr && __s2sdk_SetMenuCloseOnSelect != nullptr &&
		   __s2sdk_SetMenuType != nullptr && __s2sdk_GetMenuType != nullptr &&
		   __s2sdk_GetClientSteamID64 != nullptr;
}

void StartMenuLayer(void* user, MenuOwnerEligibleHandler eligible) {
	g_actions.clear();
	g_shows.clear();
	g_closes.clear();
	g_destroy.clear();
	for (OpenMenu& state : g_open) {
		state = OpenMenu{};
	}
	g_inHandler = false;
	g_bindingsErrorLogged = false; // one line per plugin instance
	g_user = user;
	g_eligible = eligible;

	if (!MenuBindingsAvailable()) {
		// No reserved id 0 either: without the teardown bindings no menu of this plugin may exist.
		ReportMissingBindings();
		return;
	}
	if (g_reservedIdZero) {
		return;
	}
	// s2sdk's MenuManager starts ids at 0 but also uses 0 for "no menu", so the first menu created
	// in the process could never be displayed/tracked. Create and keep one dummy menu if we got id 0
	// (cheap; done once per process by whichever plugin runs first).
	try {
		const s2sdk::MenuId id = s2sdk::CreateMenu(plg::string("admin_system:reserved-id-0"), &MenuHandler,
												   plg::string("chat"));
		if (id != 0) {
			// id 0 was already taken elsewhere: nothing to keep.
			s2sdk::DestroyMenu(id);
		} else {
			g_reservedIdZero = true;
		}
	} catch (...) {
	}
}

void ProcessMenuRequests() {
	if (g_inHandler) {
		return; // never called from a handler; the queue is drained from OnPluginUpdate
	}
	if (!IsMapReady()) {
		// The changelevel window: no client, menu display or schema call. Ended menus are still
		// retried through the safe Disconnect/destroy path so they do not stay in the manager.
		g_actions.clear();
		g_shows.clear();
		g_closes.clear();
		DestroyQueuedMenus(false);
		return;
	}

	// Actions first: a selection runs the flow (which only queues its follow-up menus), then the
	// ended menus are freed, then the queued closes and displays are applied - all outside the menu
	// manager's lock.
	std::vector<PendingAction> actions = std::move(g_actions);
	g_actions.clear();
	for (const PendingAction& action : actions) {
		ApplyAction(action);
	}
	DestroyQueuedMenus(false);

	std::vector<PendingClose> closes = std::move(g_closes);
	g_closes.clear();
	for (const PendingClose& close : closes) {
		if (!OwnerMatches(close.owner)) {
			continue; // the slot belongs to someone else now: their menu is not ours to close
		}
		DestroySlotMenu(close.owner.slot);
	}

	std::vector<PendingShow> shows = std::move(g_shows);
	g_shows.clear();
	for (PendingShow& show : shows) {
		ApplyShow(show);
	}
	DestroyQueuedMenus(false);
}

void CloseMenusOnMapEnd() {
	CloseAllNoMap();
}

void ShutdownMenuLayer() {
	g_actions.clear();
	g_shows.clear();
	g_closes.clear();
	if (IsMapReady() && MenuBindingsAvailable()) {
		// Mid-map unload (plg reload): DestroyMenu closes the backend display, which is safe while
		// the map and the client list exist - and it hides the HUD menu properly.
		for (int32_t slot = 0; slot < kMaxSlots; ++slot) {
			if (g_open[static_cast<std::size_t>(slot)].id != 0) {
				DestroySlotMenu(slot);
			}
		}
		DestroyQueuedMenus(true);
	} else {
		// Map down (or bindings incomplete): cancel-confirmed first, then the builtin no-op-close
		// type escape; a menu neither path can remove is reported as A7_UNRESOLVED.
		CloseAllNoMap();
		if (!g_destroy.empty()) {
			DestroyQueuedMenus(false);
			if (!g_destroy.empty()) {
				LogError("admin_system: A7_UNRESOLVED: could not unload a menu safely (cancel "
						 "unconfirmed and the builtin-type fallback failed) - it stays queued instead "
						 "of forcing an unsafe backend close");
			}
		}
	}
	ReleaseReservedMenuId();
	g_user = nullptr;
	g_eligible = nullptr;
}

bool IsMenuOpen(int32_t slot) {
	if (!IsSlotInRange(slot)) {
		return false;
	}
	const s2sdk::MenuId id = g_open[static_cast<std::size_t>(slot)].id;
	if (id == 0 || !IsMapReady() || __s2sdk_GetClientMenu == nullptr) {
		return false;
	}
	try {
		return s2sdk::GetClientMenu(slot) == id;
	} catch (...) {
		return false;
	}
}

std::string ResolveMenuTypeName(int32_t slot) {
	const bool hudRegistered = IsTypeRegistered("hud");
	const bool wasdRegistered = IsTypeRegistered("wasd");
	const std::string playerChoice = IsSlotInRange(slot) ? PlayerTypeChoice(slot) : std::string();
	// The admin core never forces a type (pisexcompat's per-slot SetMenuType was never called by it)
	// and always displays HUD_LAYOUT menus.
	return std::string(ResolveS2sdkMenuTypeName(/*forced=*/false, kDefaultMenuKind, playerChoice,
												hudRegistered, wasdRegistered));
}

} // namespace admin_system
