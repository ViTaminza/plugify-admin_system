// Tests for the engine-free menu decisions of plugins/admin_system (src/admin_menu_type.hpp).
//
// The two tables below are the *visible* behaviour of the Pisex admin menu and must match
// pisexcompat's ResolveS2sdkMenuTypeName()/menu trampoline exactly:
//   * the admin core displays HUD_LAYOUT menus and never forces a type, so the player's own
//     "!menu" choice (hudmenu) wins; without it HUD_LAYOUT maps to "hud" when the Panorama backend
//     is registered and to the built-in "button" otherwise;
//   * the Pisex callback only ever saw item numbers 1..5 (plus 7 = back, 9 = exit), so s2sdk's
//     index has to be translated back to the visible page button before the flow acts on it.
//
// Run by ./test.sh (build/tests/admin_menu_tests).

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "admin_command_args.hpp"
#include "admin_menu_type.hpp"

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

#define CHECK_EQ_STR(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const std::string a_{actual};                                                              \
		const std::string e_{expected};                                                            \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__, a_.c_str(), e_.c_str()); \
		}                                                                                          \
	} while (false)

#define CHECK_EQ_INT(actual, expected)                                                             \
	do {                                                                                           \
		++g_checks;                                                                                \
		const long long a_ = static_cast<long long>(actual);                                       \
		const long long e_ = static_cast<long long>(expected);                                     \
		if (a_ != e_) {                                                                            \
			++g_failures;                                                                          \
			std::printf("FAIL %s:%d: got %lld, expected %lld\n", __FILE__, __LINE__, a_, e_);      \
		}                                                                                          \
	} while (false)

using namespace admin_system;

void TestMenuTypeTable() {
	// HUD_LAYOUT (the admin core's type) with no player choice: "hud" only when the backend exists.
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::HudLayout, "", true, true), "hud");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::HudLayout, "", false, true), "button");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::HudLayout, "", false, false), "button");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, kDefaultMenuKind, "", true, false), "hud");

	// The other mapped types (Pisex MenuType -> s2sdk type).
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::Chat, "", true, true), "chat");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::Center, "", true, true), "centerhtml");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::CenterWasd, "", true, true), "wasd");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::CenterWasd, "", true, false), "button");

	// The player's own "!menu" choice wins over the mapped type; the digit backends are always
	// accepted, "hud"/"wasd" only while the matching backend is registered.
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::HudLayout, "chat", true, true), "chat");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::HudLayout, "centerhtml", true, true), "centerhtml");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::HudLayout, "button", true, true), "button");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::Chat, "hud", true, true), "hud");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::Chat, "hud", false, true), "button");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::Chat, "wasd", true, true), "wasd");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::Chat, "wasd", true, false), "button");

	// An unknown answer falls through to the mapped type instead of reaching s2sdk.
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::HudLayout, "panorama", true, true), "hud");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::HudLayout, "panorama", false, false), "button");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(false, MenuKind::Chat, "bogus", true, true), "chat");

	// A forced type (pisexcompat's SetMenuType) beats the player's choice; the admin core never
	// forces one, the flag only keeps the table complete.
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(true, MenuKind::Chat, "hud", true, true), "chat");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(true, MenuKind::HudLayout, "button", true, true), "hud");
	CHECK_EQ_STR(ResolveS2sdkMenuTypeName(true, MenuKind::HudLayout, "button", false, false), "button");

	// The library defaults of the original.
	CHECK_EQ_INT(static_cast<int>(kDefaultMenuKind), 3); // HUD_LAYOUT
	CHECK_EQ_INT(kItemsPerPage, 5);
	CHECK_EQ_INT(kMenuTimeoutSeconds, 60);
}

void TestVisibleButton() {
	// Page 1 (offset 0): s2sdk indices 0..4 are the Pisex buttons 1..5.
	for (int32_t index = 0; index < 5; ++index) {
		CHECK_EQ_INT(VisibleMenuButton(index, 0, kItemsPerPage), index + 1);
	}
	// s2sdk accepts digits 6/7 even when the page shows five items: they select indices 5/6 of the
	// *next* page and must be ignored (the original callback never saw them).
	CHECK_EQ_INT(VisibleMenuButton(5, 0, kItemsPerPage), 0);
	CHECK_EQ_INT(VisibleMenuButton(6, 0, kItemsPerPage), 0);
	// Page 2 (offset 5): index 5 is button 1 again.
	for (int32_t index = 5; index < 10; ++index) {
		CHECK_EQ_INT(VisibleMenuButton(index, 5, kItemsPerPage), index - 5 + 1);
	}
	CHECK_EQ_INT(VisibleMenuButton(4, 5, kItemsPerPage), 0); // a stale index of page 1
	// Out-of-range input never produces a button.
	CHECK_EQ_INT(VisibleMenuButton(-1, 0, kItemsPerPage), 0);
	CHECK_EQ_INT(VisibleMenuButton(0, -1, kItemsPerPage), 0);
	CHECK_EQ_INT(VisibleMenuButton(0, 0, 0), 0);
}

void TestCommandContent() {
	// A quoted multi-word argument survives the s2sdk -> Pisex content rebuild.
	plg::vector<plg::string> args;
	args.emplace_back("ban");
	args.emplace_back("Gabe Newell");
	args.emplace_back("60");
	args.emplace_back("cheating");
	const std::string content = BuildCommandContent("!ban", args);
	CHECK_EQ_STR(content, "!ban \"Gabe Newell\" 60 cheating");
	CommandArgs parsed(content.c_str());
	CHECK_EQ_INT(parsed.ArgC(), 4);
	CHECK_EQ_STR(parsed.Arg(0), "!ban");
	CHECK_EQ_STR(parsed.Arg(1), "Gabe Newell");
	CHECK_EQ_STR(parsed.Arg(2), "60");
	CHECK_EQ_STR(parsed.Arg(3), "cheating");

	// Without arguments only the spelling is handed back (mm_admin).
	plg::vector<plg::string> noArgs;
	noArgs.emplace_back("mm_admin");
	CHECK_EQ_STR(BuildCommandContent("mm_admin", noArgs), "mm_admin");
	CHECK_EQ_STR(BuildCommandContent("mm_admin", plg::vector<plg::string>{}), "mm_admin");

	// An empty token is quoted so the argument count is preserved.
	plg::vector<plg::string> withEmpty;
	withEmpty.emplace_back("add_admin");
	withEmpty.emplace_back("");
	withEmpty.emplace_back("Nick");
	CHECK_EQ_STR(BuildCommandContent("!add_admin", withEmpty), "!add_admin \"\" Nick");
	CommandArgs emptyToken(BuildCommandContent("!add_admin", withEmpty).c_str());
	CHECK_EQ_INT(emptyToken.ArgC(), 3);
	CHECK_EQ_STR(emptyToken.Arg(1), "");
}

} // namespace

int main() {
	TestMenuTypeTable();
	TestVisibleButton();
	TestCommandContent();

	if (g_failures != 0) {
		std::printf("admin_menu_tests: %d/%d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("admin_menu_tests: all %d checks passed\n", g_checks);
	return 0;
}
