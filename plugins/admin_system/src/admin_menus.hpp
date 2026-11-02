// admin_system — the native s2sdk menu layer of the admin menu.
//
// The original built a Pisex menu (cs2-menus) for every screen and pisexcompat translated it onto
// s2sdk's menu manager. This layer talks to the s2sdk menu API directly and keeps the visible
// behaviour:
//
//   * five items per page (cs2-menus pagination), an optional exit button and an optional
//     exit-back button,
//   * close-on-select is OFF: the original callback decided what happened after a selection,
//   * selecting a visible item reports the Pisex item number 1..5 plus the item's `back` value and
//     display text; the exit-back button reports ("back", "back", 7) and the exit button
//     ("exit", "exit", 9) - the exact values the Pisex callback received,
//   * the menu type follows the player's own "!menu" choice through the optional hudmenu plugin
//     (hudmenu::GetClientMenuType) and falls back to the HUD_LAYOUT mapping: "hud" when the
//     Panorama backend is registered, else the built-in "button" (admin_menu_type.hpp has the
//     table),
//   * a 60 s timeout (cs2-menus "MenuTime", pisexcompat's default).
//
// Lifecycle / safety rules s2sdk leaves to the plugin (PITFALLS A7, C10, C13):
//   * EVERY menu action is queued and applied from OnPluginUpdate: displays, closes and the
//     selection callbacks never run inside s2sdk's menu handler (which holds the menu manager's
//     recursive mutex), so a handler can never re-enter the manager, and the database, client and
//     menu calls of the flows all happen on the plugin's own pump,
//   * every request carries the verified OwnerIdentity (SteamID64 + controller handle/generation)
//     captured when it was queued; ApplyShow/ApplyAction re-capture and compare it - and ask the
//     plugin whether the slot's admin data is authoritative (IsClientAdminDataReady) - before any
//     menu is created or a callback runs, so a request is never applied to a player that re-used
//     the slot,
//   * the complete menu binding set (create/add/display/get/cancel/destroy, the display setters,
//     set/get menu type and GetClientSteamID64) must be present BEFORE the first creation,
//     including the reserved MenuId 0. Without it no menu is created at all, so no handler can be
//     left behind,
//   * s2sdk NEVER destroys a menu by itself: every ended display (select/replace, exit, back,
//     timeout, disconnect) is queued and destroyed in ProcessMenuRequests() on a later
//     OnPluginUpdate, outside the menu manager's mutex,
//   * s2sdk uses MenuId 0 for "no menu" but assigns it to the first menu of the process, so
//     StartMenuLayer() burns it with a dummy menu (pisexcompat did the same) and
//     ShutdownMenuLayer() destroys it before the library unloads,
//   * at map end / unload while the map is down a displayed session is first ended with
//     CancelClientMenu(Disconnect) - s2sdk skips the menu backend for that reason. DestroyMenu is
//     called only after the client state no longer references the id. If the cancel cannot be
//     confirmed, the menu is switched to the built-in "chat"/"console" backend (SetMenuType +
//     GetMenuType verification; ChatMenu_Close/ConsoleMenu_Close are no-ops) and destroyed through
//     it, so no client call happens and no handler pointer stays in the manager (PITFALLS B2/C10,
//     A7). A menu that even that escape cannot remove is reported as A7_UNRESOLVED at unload and is
//     never destroyed with an unsafe backend close.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "admin_guards.hpp"
#include "admin_menu_type.hpp"

namespace admin_system {

// One added menu item (the Pisex Items struct): the value the callback receives as `back`, the
// text the player sees and whether the entry is selectable. Named MenuEntry because MenuItem is
// the admin menu *registry* entry of the plugin (admin_plugin.hpp).
struct MenuEntry {
	std::string back;
	std::string text;
	bool disabled = false;
};

// A menu under construction. The builder calls mirror the Pisex ones (SetTitleMenu, AddItemMenu,
// SetExitMenu, SetBackMenu, SetCallback), the display is deferred.
struct Menu {
	std::string title;
	std::vector<MenuEntry> items;
	bool exitButton = false;
	bool backButton = false;
	// (back value, display text, Pisex item number 1..5 / 7 / 9, slot) - the Pisex callback.
	std::function<void(const char* back, const char* front, int item, int slot)> callback;
};

// The builder (Pisex AddItemMenu/SetTitleMenu/SetExitMenu/SetBackMenu/SetCallback). A disabled
// item is shown but cannot be selected.
void SetMenuTitle(Menu& menu, std::string title);
void AddMenuItem(Menu& menu, std::string back, std::string text, bool disabled = false);
void SetMenuExitButton(Menu& menu, bool enabled);
void SetMenuExitBackButton(Menu& menu, bool enabled);
void SetMenuCallback(Menu& menu, std::function<void(const char*, const char*, int, int)> callback);

// Queues the display for this slot. The owner is captured here; when the slot has no verified
// owner the request is dropped (a menu must never be shown to an unverified client).
void DisplayMenu(Menu menu, int32_t slot);
// Queues "close whatever this slot has open" (no callback, like Pisex's ClosePlayerMenu).
void CloseMenu(int32_t slot);

// --- lifecycle ---------------------------------------------------------------------------------

// The whole menu lifecycle depends on this binding set; false disables menus entirely.
bool MenuBindingsAvailable();

// The eligibility hook: true when the verified owner may still see/receive the admin menu. The
// plugin answers with OwnerMatches() plus "the slot's admin data is authoritative".
using MenuOwnerEligibleHandler = bool (*)(void* user, const OwnerIdentity& owner);

// Resets the state, installs the hooks and burns MenuId 0 (when the bindings are complete).
void StartMenuLayer(void* user, MenuOwnerEligibleHandler eligible);

// Applies the queued actions, destroys the ended menus and then creates the queued displays; called
// from OnPluginUpdate (never from a menu handler).
void ProcessMenuRequests();

// Map end: drops the queues and removes every menu through the cancel-confirmed / builtin-type
// escape path (no client call may happen with the map down).
void CloseMenusOnMapEnd();

// OnPluginEnd: destroys every menu, drops the queues and releases the reserved id 0. Reports
// A7_UNRESOLVED when a menu could not be removed safely.
void ShutdownMenuLayer();

// True while this plugin still has a menu displayed to the slot.
bool IsMenuOpen(int32_t slot);

// The s2sdk menu type this slot's menus are rendered with (admin_menu_type.hpp).
std::string ResolveMenuTypeName(int32_t slot);

} // namespace admin_system
