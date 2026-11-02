// admin_system — the engine-free decisions of the menu layer.
//
// The menu presentation is the part of the port that must stay *visibly* identical to the Pisex
// original while the code underneath is s2sdk's menu manager. The two rules that decide what a
// player sees are pure functions so they are covered by
// plugins/admin_system/tests/admin_menu_tests.cpp without an engine:
//
//   * ResolveS2sdkMenuTypeName() is the exact table of pisexcompat's
//     ResolveS2sdkMenuTypeName() (libs/pisexcompat/src/menu.cpp). The admin core always displays
//     Pisex's HUD_LAYOUT menus and never forces a type, so the table decides between the
//     hudmenu "hud" backend (the on-screen Panorama menu), the players' own "!menu" choice and
//     the built-in "button" fallback:
//         choice "chat"/"centerhtml"/"button" -> that type;
//         choice "hud"  -> "hud" when the backend is registered, else "button";
//         choice "wasd" -> "wasd" when the backend is registered, else "button";
//         no/empty choice -> HUD_LAYOUT -> "hud" when registered, else "button".
//     A forced type (pisexcompat's per-slot SetMenuType) wins over everything; this plugin never
//     forces one, but the flag stays part of the table so the parity is complete.
//   * VisibleMenuButton() translates s2sdk's global item index back into the Pisex item number
//     (1..5 for the current page, 0 for "not a visible item"). s2sdk accepts digits 1..7 even when
//     the page shows five items and maps them to currentOffset + digit - 1, so 6/7 would select an
//     item of the next page that was never displayed; the original callback only ever saw 1..5.

#pragma once

#include <cstdint>
#include <string_view>

namespace admin_system {

// cs2-menus MenuType (the Pisex numbering).
enum class MenuKind : int32_t {
	Chat = 0,
	Center = 1,
	CenterWasd = 2,
	HudLayout = 3,
};

// The menu type the admin core displays (pisexcompat's default: HUD_LAYOUT).
inline constexpr MenuKind kDefaultMenuKind = MenuKind::HudLayout;
// cs2-menus shows five items per chat page; the admin core inherited that page size.
inline constexpr int32_t kItemsPerPage = 5;
// cs2-menus "MenuTime" default of pisexcompat (the original admin core did not read core.ini's
// MenuTime; the library default was 60 s).
inline constexpr int32_t kMenuTimeoutSeconds = 60;

inline std::string_view MappedMenuTypeName(MenuKind kind, bool hudTypeRegistered, bool wasdTypeRegistered) {
	switch (kind) {
	case MenuKind::Chat: return "chat";
	case MenuKind::Center: return "centerhtml";
	// Pisex's CENTER_WASD: the hudmenu "wasd" backend; s2sdk's built-in "button" (a WASD-driven
	// center HTML panel) is the fallback while it is not registered.
	case MenuKind::CenterWasd: return wasdTypeRegistered ? "wasd" : "button";
	case MenuKind::HudLayout: return hudTypeRegistered ? "hud" : "button"; // without the backend, WASD is closest
	}
	return "chat";
}

// The exact table of pisexcompat::ResolveS2sdkMenuTypeName (see the header comment).
inline std::string_view ResolveS2sdkMenuTypeName(bool forced, MenuKind kind, std::string_view playerTypeChoice,
												 bool hudTypeRegistered, bool wasdTypeRegistered) {
	if (!forced && !playerTypeChoice.empty()) {
		// hudmenu's answer: only the known names are accepted, and "hud"/"wasd" only while the
		// matching backend is really registered (a stale/foreign answer falls through to the
		// mapped type).
		if (playerTypeChoice == "chat" || playerTypeChoice == "centerhtml" || playerTypeChoice == "button") {
			return playerTypeChoice;
		}
		if (playerTypeChoice == "hud") {
			return hudTypeRegistered ? "hud" : "button";
		}
		if (playerTypeChoice == "wasd") {
			return wasdTypeRegistered ? "wasd" : "button";
		}
	}
	return MappedMenuTypeName(kind, hudTypeRegistered, wasdTypeRegistered);
}

// s2sdk's global item index -> the Pisex item number of the visible page (1..5), or 0 when the
// index belongs to another page (or the numbers are inconsistent).
inline int32_t VisibleMenuButton(int32_t itemIndex, int32_t pageOffset, int32_t itemsPerPage) {
	if (itemIndex < 0 || pageOffset < 0 || itemsPerPage <= 0 || itemIndex < pageOffset) {
		return 0;
	}
	const int32_t button = itemIndex - pageOffset + 1;
	if (button < 1 || button > itemsPerPage) {
		return 0;
	}
	return button;
}

} // namespace admin_system
