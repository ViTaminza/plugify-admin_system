// admin_system — the admin menu, ported from ref/pisex/cs2-admin_system/menus.cpp.
//
//   main menu          categories (sorting.ini order), each with its display callback
//   category menu      the items of a category (sorting.ini order), filtered by permission
//   item               the item's select callback (registered by the modules)
//   punishments        punish / unpunish / punish offline / unpunish offline, the flows of the
//                      original: select a punishment type, a player (or an offline steamid), a
//                      reason (or a typed one) and a time, then apply it
//
// The registry lives here (the mCategories/mItems maps of the original), the menu plumbing comes
// from libs/pisexcompat. Menu callbacks run under s2sdk's menu lock, so nothing inside them calls
// the menu subsystem: pisexcompat queues DisplayPlayerMenu/ClosePlayerMenu from a callback and
// applies them in the next Poll().

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pisex/pisex.hpp>

#include "admin_plugin.hpp"

namespace admin_system {
namespace {

constexpr const char* kTypePhrases[kPunishTypeCount] = {"Item_BanPlayer", "Item_MutePlayer", "Item_GagPlayer",
														"Item_SilencePlayer"};
constexpr const char* kUnpunishPhrases[kPunishTypeCount] = {"Item_UnBanPlayer", "Item_UnMutePlayer", "Item_UnGagPlayer",
															"Item_UnSilencePlayer"};
constexpr const char* kPermissions[kPunishTypeCount] = {"@admin/ban", "@admin/mute", "@admin/gag", "@admin/silence"};
constexpr const char* kUnpunishPermissions[kPunishTypeCount] = {"@admin/unban", "@admin/unmute", "@admin/ungag",
																"@admin/unsilence"};

// The menu item numbers pisexcompat reports: 1..5 select an entry, 7 = exit-back, 9 = exit.
inline constexpr int kMenuSelectMax = 7;

} // namespace

// --- registry helpers -----------------------------------------------------------------------

const MenuItem* AdminPlugin::FindItem(const std::string& category, const std::string& identity) const {
	const auto categoryIt = m_categories.find(category);
	if (categoryIt == m_categories.end()) {
		return nullptr;
	}
	const auto itemIt = categoryIt->second.items.find(identity);
	return itemIt == categoryIt->second.items.end() ? nullptr : &itemIt->second;
}

bool AdminPlugin::HasAccessInItem(int32_t slot, const std::string& category, const std::string& identity) const {
	const AdminCore* core = CoreOrNull();
	if (core == nullptr) {
		return false;
	}
	const MenuItem* item = FindItem(category, identity);
	if (item == nullptr || item->flags.empty()) {
		return false;
	}
	return std::any_of(item->flags.begin(), item->flags.end(),
					   [core, slot](const std::string& flag) { return core->HasPermission(slot, flag); });
}

bool AdminPlugin::HasAccessInCategory(int32_t slot, const std::string& category) const {
	const auto categoryIt = m_categories.find(category);
	if (categoryIt == m_categories.end()) {
		return false;
	}
	return std::any_of(categoryIt->second.items.begin(), categoryIt->second.items.end(),
					   [this, slot, &category](const auto& item) { return HasAccessInItem(slot, category, item.first); });
}

// The label of a category/item: the phrase of its name, run through the module's display callback
// (which may rewrite it, e.g. to append a player count).
std::string AdminPlugin::CategoryLabel(int32_t slot, const MenuCategory& category) const {
	plg::string label(Phrase(category.name.c_str()));
	if (category.display != nullptr) {
		category.display(slot, plg::string(category.identity), label);
	}
	return std::string(std::string_view(label));
}

std::string AdminPlugin::ItemLabel(int32_t slot, const MenuItem& item) const {
	plg::string label(Phrase(item.name.c_str()));
	if (item.display != nullptr) {
		item.display(slot, plg::string(item.category), plg::string(item.identity), label);
	}
	return std::string(std::string_view(label));
}

// --- the menu ---------------------------------------------------------------------------------

void AdminPlugin::ShowAdminMenu(int32_t slot) {
	// PARITY: the original returned early for a player that is not an admin. The console (-1) has
	// no menu, so only real slots get this far (the original handed -1 to the menu API).
	if (slot < 0 || slot >= kMaxSlots || CoreOrNull() == nullptr || !IsAdminSlot(slot)) {
		return;
	}
	pisex::Menu menu;
	pisex::g_pMenus->SetTitleMenu(menu, Phrase("MainTitle"));

	// The categories of sorting.ini first, then the rest in registry order.
	const auto addCategory = [&](const std::string& key) {
		const auto it = m_categories.find(key);
		if (it == m_categories.end()) {
			return;
		}
		if (!HasAccessInCategory(slot, key)) {
			return;
		}
		pisex::g_pMenus->AddItemMenu(menu, key.c_str(), CategoryLabel(slot, it->second).c_str());
	};
	for (const std::string& key : m_sortCategories) {
		addCategory(key);
	}
	for (const auto& [key, category] : m_categories) {
		if (std::find(m_sortCategories.begin(), m_sortCategories.end(), key) == m_sortCategories.end()) {
			addCategory(key);
		}
	}

	pisex::g_pMenus->SetExitMenu(menu, true);
	pisex::g_pMenus->SetBackMenu(menu, false);
	pisex::g_pMenus->SetCallback(menu, [](const char* back, const char*, int item, int playerSlot) {
		try {
			if (item < kMenuSelectMax && back != nullptr) {
				g_adminPlugin.SetLastCategory(playerSlot, back);
				g_adminPlugin.ShowAdminCategoryMenu(playerSlot, back);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in the admin menu callback: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in the admin menu callback");
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
}

void AdminPlugin::ShowAdminCategoryMenu(int32_t slot, const std::string& category) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	const auto categoryIt = m_categories.find(category);
	if (categoryIt == m_categories.end()) {
		return;
	}
	pisex::Menu menu;
	pisex::g_pMenus->SetTitleMenu(menu, Phrase(categoryIt->second.name.c_str()));

	const auto addItem = [&](const std::string& identity) {
		const auto itemIt = categoryIt->second.items.find(identity);
		if (itemIt == categoryIt->second.items.end()) {
			return;
		}
		const MenuItem& item = itemIt->second;
		// PARITY: the original skipped items without a category/identity and without access.
		if (item.identity.empty() || item.category.empty() || !HasAccessInItem(slot, category, identity)) {
			return;
		}
		pisex::g_pMenus->AddItemMenu(menu, identity.c_str(), ItemLabel(slot, item).c_str());
	};
	if (const auto sortIt = m_sortItems.find(category); sortIt != m_sortItems.end()) {
		for (const std::string& identity : sortIt->second) {
			addItem(identity);
		}
	}
	for (const auto& [identity, item] : categoryIt->second.items) {
		if (const auto sortIt = m_sortItems.find(category);
			sortIt != m_sortItems.end() &&
			std::find(sortIt->second.begin(), sortIt->second.end(), identity) != sortIt->second.end()) {
			continue;
		}
		addItem(identity);
	}

	pisex::g_pMenus->SetExitMenu(menu, true);
	pisex::g_pMenus->SetBackMenu(menu, true);
	pisex::g_pMenus->SetCallback(menu, [category](const char* back, const char*, int item, int playerSlot) {
		try {
			if (item < kMenuSelectMax && back != nullptr) {
				g_adminPlugin.ShowAdminItemMenu(playerSlot, category, back);
			} else if (item == kMenuSelectMax) {
				g_adminPlugin.ShowAdminMenu(playerSlot);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in the category menu callback: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in the category menu callback");
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
}

void AdminPlugin::ShowAdminLastCategoryMenu(int32_t slot) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	const std::string& category = m_lastCategory[static_cast<size_t>(slot)];
	if (category.empty()) {
		return;
	}
	ShowAdminCategoryMenu(slot, category);
}

void AdminPlugin::ShowAdminItemMenu(int32_t slot, const std::string& category, const std::string& identity) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	const MenuItem* item = FindItem(category, identity);
	if (item == nullptr || item->select == nullptr) {
		return;
	}
	item->select(slot, plg::string(category), plg::string(identity), plg::string(item->name));
}

void AdminPlugin::CloseAdminMenu(int32_t slot) const {
	pisex::g_pMenus->ClosePlayerMenu(slot);
}

// --- the punishment flows (ShowPunishMenu / ShowUnPunishMenu / ... of the original) -----------

void AdminPlugin::ShowPunishTypeMenu(int32_t slot, bool offline, bool unpunish) {
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}
	pisex::Menu menu;
	pisex::g_pMenus->SetTitleMenu(menu, Phrase(unpunish ? "SelectUnPunishType" : "SelectPunishType"));
	const char* const* phrases = unpunish ? kUnpunishPhrases : kTypePhrases;
	const char* const* permissions = unpunish ? kUnpunishPermissions : kPermissions;
	for (int type = 0; type < kPunishTypeCount; ++type) {
		const AdminCore* core = CoreOrNull();
		if (core == nullptr || !core->HasPermission(slot, permissions[type])) {
			continue;
		}
		pisex::g_pMenus->AddItemMenu(menu, std::to_string(type).c_str(), Phrase(phrases[type]));
	}
	pisex::g_pMenus->SetExitMenu(menu, true);
	pisex::g_pMenus->SetBackMenu(menu, true);
	pisex::g_pMenus->SetCallback(menu, [offline, unpunish](const char* back, const char*, int item, int playerSlot) {
		try {
			if (item < kMenuSelectMax && back != nullptr) {
				const int type = std::atoi(back);
				if (unpunish) {
					if (offline) {
						g_adminPlugin.ShowUnpunishOfflineMenu(playerSlot, type);
					} else {
						g_adminPlugin.ShowUnpunishMenu(playerSlot, type);
					}
				} else if (offline) {
					g_adminPlugin.ShowPunishOfflineMenu(playerSlot, type);
				} else {
					g_adminPlugin.ShowPunishMenu(playerSlot, type);
				}
			} else if (item == kMenuSelectMax) {
				g_adminPlugin.ShowAdminLastCategoryMenu(playerSlot);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in the punishment type menu: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in the punishment type menu");
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
}

void AdminPlugin::ShowPunishMenu(int32_t slot, int32_t type) {
	// PARITY: the immunity_type rules decide which players the menu offers (the same rules the
	// original applied when the entry was added).
	if (slot < 0 || slot >= kMaxSlots || type < 0 || type >= kPunishTypeCount) {
		return;
	}
	const AdminCore* core = CoreOrNull();
	if (core == nullptr) {
		return;
	}
	const int immunity = core->FindAdmin(slot) == nullptr ? 0 : core->FindAdmin(slot)->immunity;
	pisex::Menu menu;
	pisex::g_pMenus->SetTitleMenu(menu, Phrase("SelectPlayer"));
	bool found = false;
	for (int32_t target = 0; target < kMaxSlots; ++target) {
		if (!pisex::g_pPlayers->IsConnected(target) || pisex::g_pPlayers->GetSteamID64(target) == 0) {
			continue;
		}
		if (pisex::GetPawnEntity(target) == 0) {
			continue; // PARITY: the original skipped a slot whose pawn handle is null (GetPlayerPawn())
		}
		if (core->IsPunished(target, type)) {
			continue;
		}
		const int targetImmunity = core->FindAdmin(target) == nullptr ? 0 : core->FindAdmin(target)->immunity;
		switch (Cfg().immunity_type) {
		case 1:
			if (immunity < targetImmunity) {
				continue;
			}
			break;
		case 2:
			if (immunity <= targetImmunity) {
				continue;
			}
			break;
		case 3:
			if (core->IsAdmin(target)) {
				continue;
			}
			break;
		default:
			break;
		}
		pisex::g_pMenus->AddItemMenu(menu, std::to_string(target).c_str(), pisex::g_pPlayers->GetPlayerName(target));
		found = true;
	}
	if (!found) {
		pisex::g_pMenus->AddItemMenu(menu, "0", Phrase("NoPlayers"), pisex::ITEM_DISABLED);
	}
	pisex::g_pMenus->SetExitMenu(menu, true);
	pisex::g_pMenus->SetBackMenu(menu, true);
	pisex::g_pMenus->SetCallback(menu, [type](const char* back, const char*, int item, int playerSlot) {
		try {
			if (item < kMenuSelectMax && back != nullptr) {
				g_adminPlugin.ShowReasonsMenu(playerSlot, static_cast<uint64_t>(std::atoi(back)), type, false);
			} else if (item == kMenuSelectMax) {
				g_adminPlugin.ShowPunishTypeMenu(playerSlot, false, false);
			}
		} catch (...) {
			LogError("admin_system: unknown exception in the punish menu");
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
}

void AdminPlugin::ShowReasonsMenu(int32_t slot, uint64_t target, int32_t type, bool offline) {
	if (slot < 0 || slot >= kMaxSlots || type < 0 || type >= kPunishTypeCount) {
		return;
	}
	pisex::Menu menu;
	pisex::g_pMenus->SetTitleMenu(menu, Phrase("SelectReason"));
	const bool ownReasons = Cfg().own_reason && CoreOrNull() != nullptr && Core().HasPermission(slot, "@admin/own_reason");
	if (Cfg().time_reason_type != 0) {
		for (const ReasonTimes& reason : Cfg().reasons_times[static_cast<size_t>(type)]) {
			if (reason.reason == "own") {
				if (ownReasons) {
					pisex::g_pMenus->AddItemMenu(menu, "own", Phrase("Item_OwnReason"));
				}
			} else {
				pisex::g_pMenus->AddItemMenu(menu, reason.reason.c_str(), reason.reason.c_str());
			}
		}
	} else {
		if (ownReasons) {
			pisex::g_pMenus->AddItemMenu(menu, "own", Phrase("Item_OwnReason"));
		}
		for (const std::string& reason : Cfg().reasons[static_cast<size_t>(type)]) {
			pisex::g_pMenus->AddItemMenu(menu, reason.c_str(), reason.c_str());
		}
	}
	pisex::g_pMenus->SetExitMenu(menu, true);
	pisex::g_pMenus->SetBackMenu(menu, true);
	pisex::g_pMenus->SetCallback(menu, [type, target, offline](const char* back, const char*, int item, int playerSlot) {
		try {
			if (item >= kMenuSelectMax) {
				if (item == kMenuSelectMax) {
					g_adminPlugin.ShowPunishTypeMenu(playerSlot, offline, false);
				}
				return;
			}
			if (back == nullptr) {
				return;
			}
			if (std::string_view(back) == "own") {
				// The reason is typed in chat (the OnChatPre capture of the original).
				OwnReasonState& own = g_adminPlugin.OwnReason(playerSlot);
				own.active = true;
				own.target = target;
				own.type = type;
				own.offline = offline;
				g_adminPlugin.CloseAdminMenu(playerSlot);
				pisex::g_pUtils->PrintToChat(playerSlot, g_adminPlugin.Phrase("EnterOwnReason"));
				return;
			}
			g_adminPlugin.ShowTimesMenuFor(playerSlot, target, type, back, offline, "");
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in the reason menu: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in the reason menu");
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
}

void AdminPlugin::ShowTimesMenuFor(int32_t slot, uint64_t target, int32_t type, const std::string& reason, bool offline,
								   const std::string& ownReason) {
	if (slot < 0 || slot >= kMaxSlots || type < 0 || type >= kPunishTypeCount) {
		return;
	}
	pisex::Menu menu;
	pisex::g_pMenus->SetTitleMenu(menu, Phrase("SelectTime"));
	const std::string resolved = reason == "own" && !ownReason.empty() ? ownReason : reason;
	int count = 0;
	if (Cfg().time_reason_type != 0) {
		for (const ReasonTimes& entry : Cfg().reasons_times[static_cast<size_t>(type)]) {
			if (entry.reason != reason) {
				continue;
			}
			for (const auto& [seconds, label] : entry.times) {
				pisex::g_pMenus->AddItemMenu(menu, std::to_string(seconds).c_str(), label.c_str());
				++count;
			}
			break;
		}
	} else {
		for (const auto& [seconds, label] : Cfg().times[static_cast<size_t>(type)]) {
			pisex::g_pMenus->AddItemMenu(menu, std::to_string(seconds).c_str(), label.c_str());
			++count;
		}
	}
	if (count == 0) {
		pisex::g_pMenus->AddItemMenu(menu, "0", Phrase("NoTimes"), pisex::ITEM_DISABLED);
	}
	pisex::g_pMenus->SetExitMenu(menu, true);
	pisex::g_pMenus->SetBackMenu(menu, true);
	pisex::g_pMenus->SetCallback(menu, [type, target, resolved, offline](const char* back, const char*, int item,
																		int playerSlot) {
		try {
			if (item >= kMenuSelectMax) {
				if (item == kMenuSelectMax) {
					g_adminPlugin.ShowReasonsMenu(playerSlot, target, type, offline);
				}
				return;
			}
			if (back == nullptr) {
				return;
			}
			const int seconds = std::atoi(back);
			if (offline) {
				// The original marked the offline entry so the menu does not offer it again.
				const std::string name = g_adminPlugin.MarkOfflinePunished(target, type, playerSlot);
				g_adminPlugin.AddOfflinePlayerPunishment(std::to_string(target), name, type, seconds, resolved, playerSlot);
			} else {
				g_adminPlugin.AddPlayerPunishment(g_adminPlugin.IdentityOf(static_cast<int32_t>(target)), type, seconds,
												  resolved, playerSlot, true, true);
			}
			g_adminPlugin.CloseAdminMenu(playerSlot);
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in the time menu: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in the time menu");
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
}

void AdminPlugin::ShowUnpunishMenu(int32_t slot, int32_t type) {
	const AdminCore* core = CoreOrNull();
	if (slot < 0 || slot >= kMaxSlots || type < 0 || type >= kPunishTypeCount || core == nullptr) {
		return;
	}
	// PARITY: with unpunish_type 0 an admin only sees the punishments it issued itself, unless it
	// has '@admin/unpunish_all'.
	const bool unpunishAll = core->HasPermission(slot, "@admin/unpunish_all");
	const uint64_t ownSteamId = SlotSteamId(slot);
	pisex::Menu menu;
	pisex::g_pMenus->SetTitleMenu(menu, Phrase("SelectPlayer"));
	bool found = false;
	for (int32_t target = 0; target < kMaxSlots; ++target) {
		if (!pisex::g_pPlayers->IsConnected(target) || pisex::g_pPlayers->GetSteamID64(target) == 0) {
			continue;
		}
		if (!core->IsPunished(target, type)) {
			continue;
		}
		if (Cfg().unpunish_type == 0 && !unpunishAll && core->PunishmentAdminSteamId(target, type) != ownSteamId) {
			continue;
		}
		pisex::g_pMenus->AddItemMenu(menu, std::to_string(target).c_str(), pisex::g_pPlayers->GetPlayerName(target));
		found = true;
	}
	if (!found) {
		pisex::g_pMenus->AddItemMenu(menu, "0", Phrase("NoPlayers"), pisex::ITEM_DISABLED);
	}
	pisex::g_pMenus->SetExitMenu(menu, true);
	pisex::g_pMenus->SetBackMenu(menu, true);
	pisex::g_pMenus->SetCallback(menu, [type](const char* back, const char* front, int item, int playerSlot) {
		try {
			if (item < kMenuSelectMax && back != nullptr) {
				g_adminPlugin.RemovePlayerPunishment(std::atoi(back), type, playerSlot, true);
				pisex::g_pUtils->PrintToChat(playerSlot, g_adminPlugin.Phrase("UnPunishSuccess"),
											 front != nullptr ? front : "");
				g_adminPlugin.CloseAdminMenu(playerSlot);
			} else if (item == kMenuSelectMax) {
				g_adminPlugin.ShowPunishTypeMenu(playerSlot, false, true);
			}
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception in the unpunish menu: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception in the unpunish menu");
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
}

void AdminPlugin::ShowPunishOfflineMenu(int32_t slot, int32_t type) {
	if (slot < 0 || slot >= kMaxSlots || type < 0 || type >= kPunishTypeCount) {
		return;
	}
	pisex::Menu menu;
	pisex::g_pMenus->SetTitleMenu(menu, Phrase("SelectOfflinePlayer"));
	bool found = false;
	for (const OfflineUser& user : m_offlineUsers) {
		if (user.punished[static_cast<size_t>(type)] || user.admin[static_cast<size_t>(type)] != 0) {
			continue;
		}
		pisex::g_pMenus->AddItemMenu(menu, std::to_string(user.steamid).c_str(), user.name.c_str());
		found = true;
	}
	if (!found) {
		pisex::g_pMenus->AddItemMenu(menu, "0", Phrase("NoPlayers"), pisex::ITEM_DISABLED);
	}
	pisex::g_pMenus->SetExitMenu(menu, true);
	pisex::g_pMenus->SetBackMenu(menu, true);
	pisex::g_pMenus->SetCallback(menu, [type](const char* back, const char*, int item, int playerSlot) {
		try {
			if (item < kMenuSelectMax && back != nullptr) {
				uint64_t steamid = 0;
				try {
					steamid = std::stoull(back);
				} catch (const std::exception&) {
					return;
				}
				g_adminPlugin.ShowReasonsMenu(playerSlot, steamid, type, true);
			} else if (item == kMenuSelectMax) {
				g_adminPlugin.ShowPunishTypeMenu(playerSlot, true, false);
			}
		} catch (...) {
			LogError("admin_system: unknown exception in the offline punish menu");
		}
	});
	pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
}

void AdminPlugin::ShowUnpunishOfflineMenu(int32_t slot, int32_t type) {
	if (m_db == nullptr || slot < 0 || slot >= kMaxSlots || type < 0 || type >= kPunishTypeCount) {
		return;
	}
	// The original ran this query and built the menu in its callback; the menu is displayed a
	// moment later (pisexcompat queues the display when called from a callback).
	const bool unpunishAll = CoreOrNull() != nullptr && Core().HasPermission(slot, "@admin/unpunish_all");
	const int limit = Cfg().unpunish_offline_count > 0 ? Cfg().unpunish_offline_count : 20;
	std::string sql = "SELECT name, steamid FROM " + Core().TablePunishments() +
					  " WHERE punish_type = ? AND (expires > ? OR expires = 0) AND unpunish_admin_id IS NULL";
	std::vector<plgdb::Value> params{static_cast<int64_t>(type), static_cast<int64_t>(std::time(nullptr))};
	if (Cfg().unpunish_type == 0 && !unpunishAll) {
		sql += " AND admin_id = ?";
		params.emplace_back(static_cast<int64_t>(Core().FindAdmin(slot) == nullptr ? 1 : Core().FindAdmin(slot)->id));
	}
	sql += " ORDER BY id DESC LIMIT ?";
	params.emplace_back(static_cast<int64_t>(limit));

	m_db->Query(std::move(sql), std::move(params), [slot, type](const plgdb::Result& result) {
		try {
			pisex::Menu menu;
			pisex::g_pMenus->SetTitleMenu(menu, g_adminPlugin.Phrase("SelectOfflinePlayer"));
			if (result.Ok() && result.RowCount() > 0) {
				for (std::size_t row = 0; row < result.RowCount(); ++row) {
					const std::string name = result.GetString(row, 0);
					const std::string steamid = result.GetString(row, 1);
					pisex::g_pMenus->AddItemMenu(menu, steamid.c_str(), name.c_str());
				}
			} else {
				pisex::g_pMenus->AddItemMenu(menu, "0", g_adminPlugin.Phrase("NoPlayers"), pisex::ITEM_DISABLED);
			}
			pisex::g_pMenus->SetExitMenu(menu, true);
			pisex::g_pMenus->SetBackMenu(menu, true);
			pisex::g_pMenus->SetCallback(menu, [type](const char* back, const char* front, int item, int playerSlot) {
				try {
					if (item < kMenuSelectMax && back != nullptr) {
						g_adminPlugin.RemoveOfflinePlayerPunishment(back, type, playerSlot);
						pisex::g_pUtils->PrintToChat(playerSlot, g_adminPlugin.Phrase("UnPunishOfflineSuccess"),
													 front != nullptr ? front : "");
						g_adminPlugin.CloseAdminMenu(playerSlot);
					} else if (item == kMenuSelectMax) {
						g_adminPlugin.ShowPunishTypeMenu(playerSlot, true, true);
					}
				} catch (...) {
					LogError("admin_system: unknown exception in the offline unpunish menu");
				}
			});
			pisex::g_pMenus->DisplayPlayerMenu(menu, slot);
		} catch (const std::exception& ex) {
			LogError(std::string("admin_system: exception while building the offline unpunish menu: ") + ex.what());
		} catch (...) {
			LogError("admin_system: unknown exception while building the offline unpunish menu");
		}
	});
}

std::string AdminPlugin::MarkOfflinePunished(uint64_t steamid, int32_t type, int32_t admin) {
	for (OfflineUser& user : m_offlineUsers) {
		if (user.steamid != steamid) {
			continue;
		}
		user.punished[static_cast<size_t>(type)] = true;
		user.admin[static_cast<size_t>(type)] = SlotSteamId(admin);
		return user.name;
	}
	return "";
}

// --- the select callbacks of the core's own items ----------------------------------------------

void AdminSelectPunish(int32_t slot, const plg::string&, const plg::string&, const plg::string&) {
	g_adminPlugin.ShowPunishTypeMenu(slot, false, false);
}

void AdminSelectUnpunish(int32_t slot, const plg::string&, const plg::string&, const plg::string&) {
	g_adminPlugin.ShowPunishTypeMenu(slot, false, true);
}

void AdminSelectPunishOffline(int32_t slot, const plg::string&, const plg::string&, const plg::string&) {
	g_adminPlugin.ShowPunishTypeMenu(slot, true, false);
}

void AdminSelectUnpunishOffline(int32_t slot, const plg::string&, const plg::string&, const plg::string&) {
	g_adminPlugin.ShowPunishTypeMenu(slot, true, true);
}

} // namespace admin_system
