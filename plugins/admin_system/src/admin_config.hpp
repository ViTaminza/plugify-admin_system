// admin_system — the file half of the plugin: where core.ini, sorting.ini, databases.cfg,
// core.cfg and admin_system.phrases.txt are read from, and the parsers that turn them into the
// admin_core model.
//
// Self-contained (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no helper library). The Metamod
// admin core read its files as "addons/configs/admin_system/core.ini",
// "addons/configs/databases.cfg" and "addons/translations/admin_system.phrases.txt" through
// hl2sdk's KeyValues; this header keeps the file formats, the key names and - importantly - the
// *candidate order* pisexcompat used, so an installation keeps reading the file it read before
// and the first existing file stays authoritative:
//
//   1. ConfigCandidates(): the exact pisexcompat ResolvePath() order for
//      "addons/configs/admin_system/core.ini" - the raw cwd-relative file first (a Metamod tree
//      or a hand-written override next to the server binary), then
//      <configs>/admin_system/core.ini, <configs>/admin_system/translations/core.ini and
//      <data>/admin_system/core.ini. The same list is used for sorting.ini.
//   2. DatabaseCandidates(): the order for "addons/configs/databases.cfg" - the raw Metamod file,
//      then the plugin's own <configs>/admin_system/databases.cfg (the location
//      docs/PLUGIN_STANDARD.md §4 requires) and its translations/data siblings.
//   3. PhraseCandidates(): the order of pisexcompat's ResolveTranslationsPath() for the bare
//      "admin_system.phrases.txt" - the raw file, <configs>/admin_system/translations/<file>,
//      <configs>/admin_system/<file>.
//   4. Language: <configs>/core.cfg "ServerLang" first (the Metamod core setting pisexcompat
//      read), then s2sdk's ServerLanguage, then "en" - pisexcompat's DetectLanguage() chain.
//
// A malformed file is *not* a missing file: the first existing candidate is authoritative, so a
// broken core.ini keeps the built-in defaults instead of silently falling through to a decoy.
//
// Every function here is engine-free: the caller passes the plg::GetConfigsDir()/GetDataDir()
// values in and this header never touches the Plugify or s2sdk API, so it is covered by
// plugins/admin_system/tests/admin_config_tests.cpp without an engine.

#pragma once

#include "kv1.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "admin_core.hpp"

namespace admin_system {

// --- file helpers ----------------------------------------------------------------------------

inline std::string JoinPath(std::string_view base, std::string_view name) {
	if (base.empty()) {
		return std::string(name);
	}
	if (name.empty()) {
		return std::string(base);
	}
	std::string out(base);
	if (out.back() != '/') {
		out.push_back('/');
	}
	if (name.front() == '/') {
		name.remove_prefix(1);
	}
	out.append(name);
	return out;
}

inline bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

inline std::optional<std::string> ReadTextFile(const std::string& path) {
	if (path.empty()) {
		return std::nullopt;
	}
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream content;
	content << file.rdbuf();
	if (!file.good() && !file.eof()) {
		return std::nullopt;
	}
	return content.str();
}

inline std::string ToLowerText(std::string_view text) {
	std::string out(text);
	for (char& c : out) {
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}
	return out;
}

// The first existing candidate; when nothing exists the most likely location is returned so the
// caller can log a path that tells the operator where the file belongs.
inline std::string FirstExistingOrFirst(const std::vector<std::string>& candidates) {
	for (const std::string& candidate : candidates) {
		if (FileExists(candidate)) {
			return candidate;
		}
	}
	return candidates.empty() ? std::string() : candidates.front();
}

// --- candidate lists (the pisexcompat order, see the header comment) --------------------------

inline constexpr std::string_view kPluginDirName = "admin_system";
inline constexpr std::string_view kCoreIniFile = "core.ini";
inline constexpr std::string_view kSortingIniFile = "sorting.ini";
inline constexpr std::string_view kDatabasesFile = "databases.cfg";
inline constexpr std::string_view kPhrasesFile = "admin_system.phrases.txt";
inline constexpr std::string_view kCoreCfgFile = "core.cfg";
inline constexpr std::string_view kFallbackLanguage = "en";

// <configs>/admin_system - the plugin's own config directory (docs/PLUGIN_STANDARD.md §3).
inline std::string PluginConfigDir(std::string_view configsDir) {
	if (configsDir.empty()) {
		return std::string(kPluginDirName);
	}
	return JoinPath(configsDir, kPluginDirName);
}

// <configs>/admin_system/translations
inline std::string PluginTranslationsDir(std::string_view configsDir) {
	return JoinPath(PluginConfigDir(configsDir), "translations");
}

// <data>/admin_system - the only directory the plugin may write to.
inline std::string PluginDataDir(std::string_view dataDir) {
	if (dataDir.empty()) {
		return std::string(kPluginDirName);
	}
	return JoinPath(dataDir, kPluginDirName);
}

// Candidates for a file the Metamod plugin read as "addons/configs/admin_system/<file>".
inline std::vector<std::string> ConfigCandidates(std::string_view configsDir, std::string_view dataDir,
												 std::string_view file) {
	const std::string configDir = PluginConfigDir(configsDir);
	std::vector<std::string> candidates;
	const std::string raw = JoinPath("addons/configs/admin_system", file);
	candidates.push_back(raw); // as given, relative to the server working directory
	candidates.push_back(JoinPath(configDir, file));
	candidates.push_back(JoinPath(PluginTranslationsDir(configsDir), file));
	candidates.push_back(JoinPath(PluginDataDir(dataDir), file));
	return candidates;
}

// Candidates for "addons/configs/databases.cfg" (pisexcompat's ResolvePath order).
inline std::vector<std::string> DatabaseCandidates(std::string_view configsDir, std::string_view dataDir) {
	std::vector<std::string> candidates;
	candidates.emplace_back("addons/configs/databases.cfg");
	candidates.push_back(JoinPath(PluginConfigDir(configsDir), kDatabasesFile));
	candidates.push_back(JoinPath(PluginTranslationsDir(configsDir), kDatabasesFile));
	candidates.push_back(JoinPath(PluginDataDir(dataDir), kDatabasesFile));
	return candidates;
}

// Candidates for the phrases file (pisexcompat's ResolveTranslationsPath order: raw file first).
inline std::vector<std::string> PhraseCandidates(std::string_view configsDir) {
	std::vector<std::string> candidates;
	candidates.emplace_back(kPhrasesFile);
	candidates.push_back(JoinPath(PluginTranslationsDir(configsDir), kPhrasesFile));
	candidates.push_back(JoinPath(PluginConfigDir(configsDir), kPhrasesFile));
	return candidates;
}

// The SQLite file of the fail-soft path: <data>/admin_system/<name>.
inline std::string ResolveDataFilePath(std::string_view dataDir, std::string_view name) {
	return JoinPath(PluginDataDir(dataDir), name);
}

// The configured "sqlite_path": an absolute path and ":memory:" are used as given, everything else
// lives under the plugin data directory (the only place a plugin may write,
// docs/PLUGIN_STANDARD.md §2). The parent directory is created so a fresh installation works.
inline std::string ResolveSqlitePath(std::string_view dataDir, std::string_view name) {
	std::string file(name);
	if (file == ":memory:" || (!file.empty() && file.front() == '/')) {
		return file;
	}
	std::string out = ResolveDataFilePath(dataDir, file);
	std::error_code ec;
	const std::filesystem::path parent = std::filesystem::path(out).parent_path();
	if (!parent.empty()) {
		std::filesystem::create_directories(parent, ec);
	}
	return out;
}

// --- core.ini -------------------------------------------------------------------------------

// LoadConfig() of the original. A freshly constructed Config is overwritten key by key with the
// *getter* defaults of the original (which differ from the Config defaults - e.g. punish_ip is
// false when the key is missing but true in a default-constructed Config), so a file that lacks a
// key behaves exactly like the Metamod plugin. The prefix is only accepted when non-empty.
inline bool LoadCoreConfigText(std::string_view text, Config& out) {
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) {
		return false;
	}
	Config config;
	config.database_prefix = std::string(kDefaultPrefix);

	if (const std::string_view prefix = root->GetString("database_prefix", kDefaultPrefix); !prefix.empty()) {
		config.database_prefix = std::string(prefix);
	}
	config.admin_server_id = root->GetInt("admin_server_id", -1);
	config.punish_server_id = root->GetInt("punish_server_id", -1);
	config.punish_ip = root->GetBool("punish_ip", false);
	config.time_reason_type = root->GetInt("time_reason_type", 0);
	config.ban_delay = root->GetInt("ban_delay", 0);
	config.notify_type = root->GetInt("notify_type", 0);
	config.unpunish_type = root->GetInt("unpunish_type", 0);
	config.immunity_type = root->GetInt("immunity_type", 0);
	config.punish_offline_count = root->GetInt("punish_offline_count", 0);
	config.unpunish_offline_count = root->GetInt("unpunish_offline_count", 0);
	config.static_name = root->GetBool("static_name", false);
	config.message_type = root->GetInt("message_type", 0);
	config.own_reason = root->GetBool("own_reason", false);
	config.mute_ignore_commands = root->GetBool("mute_ignore_commands", true);

	// FOR_EACH_VALUE(defaults, value) of the original: every child that carries a value and is not
	// "0" is one default permission.
	if (const KvNode* defaults = root->Find("default_permissions")) {
		for (const KvNode& value : defaults->children) {
			if (value.hasValue && value.GetInt({}, 0) != 0) {
				config.default_permissions.emplace_back(value.name);
			}
		}
	}

	// FOR_EACH_SUBKEY(flags, flag): the flag name is the block name, "name" its display phrase and
	// "access" the permission list.
	if (const KvNode* flags = root->Find("flags")) {
		for (const KvNode& flag : flags->children) {
			if (!flag.isBlock) {
				continue;
			}
			Flag parsed;
			parsed.name = std::string(flag.GetString("name", ""));
			if (const KvNode* access = flag.Find("access")) {
				for (const KvNode& permission : access->children) {
					if (permission.hasValue && permission.GetInt({}, 0) != 0) {
						parsed.permissions.emplace_back(permission.name);
					}
				}
			}
			config.flags[flag.name] = std::move(parsed);
		}
	}

	// The punishment menu tables: either "reasons" + "times" (time_reason_type = 0) or
	// "times_reasons" (time_reason_type = 1), exactly as LoadConfig() read them.
	constexpr const char* kTypeKeys[kPunishmentTypes] = {"bans", "mutes", "gags", "silences"};
	if (config.time_reason_type != 0) {
		if (const KvNode* timesReasons = root->Find("times_reasons")) {
			for (int type = 0; type < kPunishmentTypes; ++type) {
				const KvNode* perType = timesReasons->Find(kTypeKeys[type]);
				if (perType == nullptr) {
					continue;
				}
				for (const KvNode& reason : perType->children) {
					if (!reason.isBlock) {
						continue;
					}
					ReasonTimes entry;
					entry.reason = reason.name;
					for (const KvNode& time : reason.children) {
						if (time.hasValue) {
							entry.times.emplace_back(std::atoi(time.name.c_str()),
													 std::string(time.GetString({})));
						}
					}
					config.reasons_times[type].push_back(std::move(entry));
				}
			}
		}
	} else {
		if (const KvNode* reasons = root->Find("reasons")) {
			for (int type = 0; type < kPunishmentTypes; ++type) {
				const KvNode* perType = reasons->Find(kTypeKeys[type]);
				if (perType == nullptr) {
					continue;
				}
				for (const KvNode& reason : perType->children) {
					if (reason.hasValue && reason.GetInt({}, 0) != 0) {
						config.reasons[type].emplace_back(reason.name);
					}
				}
			}
		}
		if (const KvNode* times = root->Find("times")) {
			for (int type = 0; type < kPunishmentTypes; ++type) {
				const KvNode* perType = times->Find(kTypeKeys[type]);
				if (perType == nullptr) {
					continue;
				}
				for (const KvNode& time : perType->children) {
					if (time.hasValue) {
						config.times[type].emplace_back(std::atoi(time.name.c_str()),
														std::string(time.GetString({})));
					}
				}
			}
		}
	}

	out = std::move(config);
	return true;
}

// The first existing candidate is authoritative: a malformed file reports false (the caller keeps
// its defaults) instead of falling through to the next candidate.
inline bool LoadCoreConfigFile(const std::vector<std::string>& candidates, Config& out, std::string& usedPath) {
	for (const std::string& candidate : candidates) {
		const std::optional<std::string> text = ReadTextFile(candidate);
		if (!text) {
			continue;
		}
		usedPath = candidate;
		return LoadCoreConfigText(*text, out);
	}
	usedPath = FirstExistingOrFirst(candidates);
	return false;
}

// --- sorting.ini ----------------------------------------------------------------------------

// The menu order of the original (g_vSortCategories / g_mSortItems): every top-level block is a
// category, every value inside it is an item identity. Values keep their duplicates and order.
inline bool LoadSortingText(std::string_view text, std::vector<std::string>& categories,
							std::map<std::string, std::vector<std::string>, std::less<>>& items) {
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) {
		return false;
	}
	categories.clear();
	items.clear();
	for (const KvNode& category : root->children) {
		if (!category.isBlock) {
			continue;
		}
		categories.push_back(category.name);
		std::vector<std::string>& list = items[category.name];
		for (const KvNode& item : category.children) {
			if (item.hasValue) {
				list.emplace_back(item.GetString({}));
			}
		}
	}
	return true;
}

inline bool LoadSortingFile(const std::vector<std::string>& candidates, std::vector<std::string>& categories,
							std::map<std::string, std::vector<std::string>, std::less<>>& items, std::string& usedPath) {
	for (const std::string& candidate : candidates) {
		const std::optional<std::string> text = ReadTextFile(candidate);
		if (!text) {
			continue;
		}
		usedPath = candidate;
		return LoadSortingText(*text, categories, items);
	}
	usedPath = FirstExistingOrFirst(candidates);
	return false;
}

// --- databases.cfg --------------------------------------------------------------------------

// The "admin_system" section of addons/configs/databases.cfg, with "driver" and "sqlite_path" as
// the extensions of this port ("driver" stays "mysql" to share one database with Metamod).
struct DatabaseSettings {
	bool found = false;
	bool sqlite = false;
	std::string host = "127.0.0.1";
	int port = 3306;
	std::string user;
	std::string password;
	std::string database;
	std::string sqlitePath; // resolved by the caller against the plugin data dir
};

inline DatabaseSettings ParseDatabaseSettings(std::string_view text) {
	DatabaseSettings settings;
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) {
		return settings;
	}
	const KvNode* entry = root->Find(kPluginDirName);
	if (entry == nullptr) {
		return settings;
	}
	settings.found = true;
	settings.sqlite = entry->GetString("driver", "mysql") == "sqlite";
	settings.host = std::string(entry->GetString("host", "127.0.0.1"));
	const int port = entry->GetInt("port", 3306);
	settings.port = std::max(1, std::min(65535, port));
	settings.user = std::string(entry->GetString("user", ""));
	settings.password = std::string(entry->GetString("pass", ""));
	settings.database = std::string(entry->GetString("database", "admin_system"));
	settings.sqlitePath = std::string(entry->GetString("sqlite_path", "admin_system.sqlite3"));
	return settings;
}

inline DatabaseSettings LoadDatabaseSettings(const std::vector<std::string>& candidates, std::string& usedPath) {
	for (const std::string& candidate : candidates) {
		const std::optional<std::string> text = ReadTextFile(candidate);
		if (!text) {
			continue;
		}
		usedPath = candidate;
		return ParseDatabaseSettings(*text);
	}
	usedPath = FirstExistingOrFirst(candidates);
	return DatabaseSettings{};
}

// --- language and phrases -------------------------------------------------------------------

// The pisexcompat NormalizeLanguage(): Source language names -> the codes of the phrase files.
inline std::string NormalizeLanguage(std::string_view name) {
	const std::string lower = ToLowerText(name);
	if (lower == "russian" || lower == "ru") return "ru";
	if (lower == "english" || lower == "en") return "en";
	if (lower == "ukrainian" || lower == "ua" || lower == "uk") return "ua";
	if (lower == "portuguese" || lower == "pt-br" || lower == "brazilian") return "pt-BR";
	if (lower == "spanish" || lower == "es") return "es";
	if (lower == "german" || lower == "de") return "de";
	if (lower == "french" || lower == "fr") return "fr";
	if (lower == "polish" || lower == "pl") return "pl";
	if (lower == "turkish" || lower == "tr") return "tr";
	if (lower == "schinese" || lower == "zh-cn") return "zh-CN";
	return std::string(name);
}

// The "ServerLang" value of <configs>/core.cfg; "" when there is no file or no such key.
// pisexcompat read exactly this file (CoreConfigLanguage() in phrases.cpp), so no other candidate
// is invented here.
inline std::string CoreConfigLanguage(std::string_view configsDir) {
	if (configsDir.empty()) {
		return {};
	}
	const std::optional<std::string> text = ReadTextFile(JoinPath(configsDir, kCoreCfgFile));
	if (!text) {
		return {};
	}
	const std::optional<KvNode> root = ParseKv1(*text);
	if (!root) {
		return {};
	}
	return std::string(root->GetString("ServerLang", ""));
}

// "core.cfg ServerLang" -> s2sdk ServerLanguage -> "en", both inputs normalized.
inline std::string PickLanguage(std::string_view coreLanguage, std::string_view sdkLanguage) {
	if (!coreLanguage.empty()) {
		return NormalizeLanguage(coreLanguage);
	}
	if (!sdkLanguage.empty()) {
		return NormalizeLanguage(sdkLanguage);
	}
	return std::string(kFallbackLanguage);
}

// Parse a *.phrases.txt into key -> text for one language, with the pisexcompat fallback rules:
// the requested language, then "en", then the first language the key has. A malformed file
// answers std::nullopt.
inline std::optional<std::map<std::string, std::string, std::less<>>> ParsePhrases(std::string_view text,
																						 std::string_view language) {
	const std::optional<KvNode> root = ParseKv1(text);
	if (!root) {
		return std::nullopt;
	}
	std::map<std::string, std::string, std::less<>> phrases;
	for (const KvNode& key : root->children) {
		if (!key.isBlock) {
			continue;
		}
		std::string phrase;
		if (!language.empty()) {
			phrase = std::string(key.GetString(language));
		}
		if (phrase.empty() && language != kFallbackLanguage) {
			phrase = std::string(key.GetString(kFallbackLanguage));
		}
		if (phrase.empty()) {
			for (const KvNode& candidate : key.children) {
				if (candidate.hasValue) {
					phrase = candidate.value;
					break;
				}
			}
		}
		phrases[key.name] = std::move(phrase);
	}
	return phrases;
}

// The first existing candidate that parses; `usedPath` receives the file that was used (or the
// most likely candidate when none exists, for the failure log).
inline std::optional<std::map<std::string, std::string, std::less<>>> LoadFirstPhrases(
	const std::vector<std::string>& candidates, std::string_view language, std::string& usedPath) {
	usedPath = FirstExistingOrFirst(candidates);
	for (const std::string& candidate : candidates) {
		const std::optional<std::string> text = ReadTextFile(candidate);
		if (!text) {
			continue;
		}
		usedPath = candidate;
		return ParsePhrases(*text, language);
	}
	return std::nullopt;
}

} // namespace admin_system
