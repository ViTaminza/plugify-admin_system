// Tests for the engine-free file half of plugins/admin_system (src/kv1.hpp, src/admin_config.hpp,
// src/admin_command_args.hpp). No engine, no plugify module: the headers are compiled exactly as
// the plugin compiles them, so the parsers cannot drift from the shipped code.
//
// Run by ./test.sh (build/tests/admin_config_tests).

#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "admin_command_args.hpp"
#include "admin_config.hpp"

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

// --- a temporary directory for the candidate-order tests -------------------------------------

struct TempDir {
	std::filesystem::path path;

	TempDir() {
		static int counter = 0;
		path = std::filesystem::temp_directory_path() /
			   ("admin_system_config_tests_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
	}
	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
	TempDir(const TempDir&) = delete;
	TempDir& operator=(const TempDir&) = delete;

	std::string Write(const std::string& relative, std::string_view content) const {
		const std::filesystem::path file = path / relative;
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		std::ofstream out(file, std::ios::binary);
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
		out.close();
		return file.string();
	}
};

// The shape of the production core.ini (ref/prod-27016/addons/configs/admin_system/core.ini): the
// block names, the flag/access nesting and the times_reasons layout the original LoadConfig() read.
constexpr std::string_view kCoreIni = R"KV(
"Config"
{
    "database_prefix"        "as_"
    "admin_server_id"        "1"
    "punish_server_id"       "-1"
    "punish_ip"              "0"
    "time_reason_type"       "1"
    "notify_type"            "2"
    "immunity_type"          "1"
    "ban_delay"              "10"
    "unpunish_type"          "0"
    "punish_offline_count"   "30"
    "unpunish_offline_count" "30"
    "message_type"           "0"
    "static_name"            "0"
    "own_reason"             "1"
    "mute_ignore_commands"   "0"

    "default_permissions"
    {
        "@admin/status"      "1"
        "@admin/votebkm"     "1"
        "@admin/disabled"    "0"
    }

    "flags"
    {
        "a"
        {
            "name"          "Резервный слот"
            "access"
            {
                "@admin/reserve"   "1"
                "@admin/spec"      "0"
            }
        }
        "b"
        {
            "name"          "Бан"
            "access"
            {
                "@admin/ban"              "1"
                "@admin/mute"             "1"
            }
        }
    }

    "times_reasons"
    {
        "bans"
        {
            "Читы"
            {
                "0"     "Навсегда"
                "3600"  "1 час"
            }
            "own"
            {
                "0"     "Навсегда"
            }
        }
        "mutes"
        {
            "Оскорбления"
            {
                "600"   "10 минут"
            }
        }
    }
}
)KV";

void TestKv1() {
	const std::optional<KvNode> root = ParseKv1(kCoreIni);
	CHECK(root.has_value());
	if (!root) {
		return;
	}
	// The single top-level block becomes the root: the keys are the ones *inside* "Config".
	CHECK_EQ_STR(root->GetString("database_prefix"), "as_");
	CHECK_EQ_INT(root->GetInt("ban_delay"), 10);
	CHECK_EQ_INT(root->GetInt("punish_server_id"), -1);
	CHECK_EQ_INT(root->GetInt("missing", 7), 7);
	CHECK(!root->GetBool("punish_ip", true));
	CHECK(!root->GetBool("static_name", true));
	CHECK(root->GetBool("own_reason", false));
	CHECK(!root->GetBool("mute_ignore_commands", true));

	// Lookups are case-insensitive (Valve's FindKey).
	CHECK_EQ_STR(root->GetString("DATABASE_PREFIX"), "as_");
	CHECK(root->Find("FLAGS") != nullptr);

	// A malformed file is rejected as a whole.
	CHECK(!ParseKv1("\"Config\" { \"a\" \"1\"").has_value()); // missing '}'
	CHECK(!ParseKv1("\"Config\" { } }").has_value());          // stray '}'
	CHECK(!ParseKv1("\"Config\" { \"a\" \"unterminated ").has_value());

	// Comments, escapes, a BOM, unquoted tokens and a value plus a block all parse.
	const std::optional<KvNode> tricky = ParseKv1(
		"\xEF\xBB\xBF"
		"// leading comment\n"
		"Root { key value /* inline */ unquoted_block { x 1 } esc \"a\\\"b\\\\c\" }\n");
	CHECK(tricky.has_value());
	if (tricky) {
		CHECK_EQ_STR(tricky->GetString("key"), "value");
		CHECK_EQ_STR(tricky->GetString("esc"), "a\"b\\c");
		CHECK(tricky->Find("unquoted_block") != nullptr);
	}

	// A value node and a block node are distinguished (the FOR_EACH_VALUE / FOR_EACH_SUBKEY split).
	const std::optional<KvNode> mixed = ParseKv1("\"R\" { \"v\" \"1\" \"b\" { \"x\" \"2\" } }");
	CHECK(mixed.has_value());
	if (mixed) {
		int values = 0;
		int blocks = 0;
		for (const KvNode& child : mixed->children) {
			if (child.hasValue) {
				++values;
			}
			if (child.isBlock) {
				++blocks;
			}
		}
		CHECK_EQ_INT(values, 1);
		CHECK_EQ_INT(blocks, 1);
	}
}

void TestCoreIni() {
	Config config;
	CHECK(LoadCoreConfigText(kCoreIni, config));
	CHECK_EQ_STR(config.database_prefix, "as_");
	CHECK_EQ_INT(config.admin_server_id, 1);
	CHECK_EQ_INT(config.punish_server_id, -1);
	CHECK(!config.punish_ip);
	CHECK_EQ_INT(config.time_reason_type, 1);
	CHECK_EQ_INT(config.ban_delay, 10);
	CHECK_EQ_INT(config.notify_type, 2);
	CHECK_EQ_INT(config.immunity_type, 1);
	CHECK_EQ_INT(config.unpunish_type, 0);
	CHECK_EQ_INT(config.punish_offline_count, 30);
	CHECK_EQ_INT(config.unpunish_offline_count, 30);
	CHECK_EQ_INT(config.message_type, 0);
	CHECK(!config.static_name);
	CHECK(config.own_reason);
	CHECK(!config.mute_ignore_commands);

	// default_permissions: only the entries set to 1 (FOR_EACH_VALUE + GetInt != 0).
	CHECK_EQ_INT(config.default_permissions.size(), 2);
	CHECK(config.IsDefaultPermission("@admin/status"));
	CHECK(config.IsDefaultPermission("@admin/votebkm"));
	CHECK(!config.IsDefaultPermission("@admin/disabled"));

	// flags: the block name is the flag, "name" the display phrase, "access" the permission list
	// (again only entries set to 1).
	CHECK_EQ_INT(config.flags.size(), 2);
	const auto flagA = config.flags.find("a");
	CHECK(flagA != config.flags.end());
	if (flagA != config.flags.end()) {
		CHECK_EQ_STR(flagA->second.name, "Резервный слот");
		CHECK_EQ_INT(flagA->second.permissions.size(), 1);
		CHECK_EQ_STR(flagA->second.permissions.front(), "@admin/reserve");
	}
	CHECK_EQ_STR(config.FlagName("b"), "Бан");
	CHECK_EQ_INT(config.PermissionsByFlag("b").size(), 2);

	// times_reasons: the reason block name is the reason, its values are (seconds, label).
	CHECK_EQ_INT(config.reasons_times[RT_BAN].size(), 2);
	if (config.reasons_times[RT_BAN].size() == 2) {
		CHECK_EQ_STR(config.reasons_times[RT_BAN][0].reason, "Читы");
		CHECK_EQ_INT(config.reasons_times[RT_BAN][0].times.size(), 2);
		CHECK_EQ_INT(config.reasons_times[RT_BAN][0].times[0].first, 0);
		CHECK_EQ_STR(config.reasons_times[RT_BAN][0].times[0].second, "Навсегда");
		CHECK_EQ_INT(config.reasons_times[RT_BAN][0].times[1].first, 3600);
		CHECK_EQ_STR(config.reasons_times[RT_BAN][1].reason, "own");
	}
	CHECK_EQ_INT(config.reasons_times[RT_MUTE].size(), 1);
	CHECK_EQ_INT(config.reasons_times[RT_GAG].size(), 0);
	// time_reason_type = 1: the "reasons"/"times" tables stay empty.
	CHECK_EQ_INT(config.reasons[RT_BAN].size(), 0);
	CHECK_EQ_INT(config.times[RT_BAN].size(), 0);

	// A malformed file keeps the caller's defaults (LoadCoreConfigText is called on a temp).
	Config broken;
	CHECK(!LoadCoreConfigText("\"Config\" { \"database_prefix\" ", broken));
	CHECK_EQ_STR(broken.database_prefix, "as_"); // a default-constructed Config

	// time_reason_type = 0 reads the "reasons" + "times" blocks instead.
	constexpr std::string_view kSplit = R"(
"Config"
{
    "time_reason_type" "0"
    "reasons"
    {
        "bans"
        {
            "Читы" "1"
            "Тимкилл" "1"
            "Скрытая" "0"
        }
    }
    "times"
    {
        "bans"
        {
            "0"     "Навсегда"
            "600"   "10 минут"
        }
    }
}
)";
	Config split;
	CHECK(LoadCoreConfigText(kSplit, split));
	CHECK_EQ_INT(split.reasons[RT_BAN].size(), 2);
	CHECK_EQ_STR(split.reasons[RT_BAN][0], "Читы");
	CHECK_EQ_INT(split.times[RT_BAN].size(), 2);
	CHECK_EQ_INT(split.times[RT_BAN][1].first, 600);
}

void TestSorting() {
	constexpr std::string_view kSorting = R"(
"Menu"
{
	"players"
	{
		"item"		"kill"
		"item"		"kick"
	}
	"punishments"
	{
		"item"		"punish"
		"item"		"unpunish"
	}
}
)";
	std::vector<std::string> categories;
	std::map<std::string, std::vector<std::string>, std::less<>> items;
	CHECK(LoadSortingText(kSorting, categories, items));
	CHECK_EQ_INT(categories.size(), 2);
	CHECK_EQ_STR(categories[0], "players");
	CHECK_EQ_STR(categories[1], "punishments");
	CHECK_EQ_INT(items["players"].size(), 2);
	CHECK_EQ_STR(items["players"][0], "kill");
	CHECK_EQ_STR(items["punishments"][1], "unpunish");

	CHECK(!LoadSortingText("\"Menu\" {", categories, items));
}

void TestDatabaseSettings() {
	constexpr std::string_view kDatabases = R"(
"Databases"
{
	"admin_system"
	{
		"host"		"127.0.0.1"
		"database"	"iksadmin"
		"user"		"mutantoff"
		"pass"		"secret"
		"port"		"3307"
	}
	"vip"
	{
		"database"	"vip"
	}
}
)";
	const DatabaseSettings settings = ParseDatabaseSettings(kDatabases);
	CHECK(settings.found);
	CHECK(!settings.sqlite);
	CHECK_EQ_STR(settings.host, "127.0.0.1");
	CHECK_EQ_STR(settings.database, "iksadmin");
	CHECK_EQ_STR(settings.user, "mutantoff");
	CHECK_EQ_STR(settings.password, "secret");
	CHECK_EQ_INT(settings.port, 3307);

	// No section: found stays false (the caller falls back to SQLite).
	const DatabaseSettings missing = ParseDatabaseSettings("\"Databases\" { \"vip\" { } }");
	CHECK(!missing.found);
	// A malformed file is "no section", never a half-read one.
	CHECK(!ParseDatabaseSettings("\"Databases\" { \"admin_system\" {").found);

	// The SQLite extension keys of this port.
	constexpr std::string_view kSqlite = R"(
"Databases"
{
	"admin_system"
	{
		"driver"	"sqlite"
		"sqlite_path"	"custom.sqlite3"
	}
}
)";
	const DatabaseSettings sqlite = ParseDatabaseSettings(kSqlite);
	CHECK(sqlite.found);
	CHECK(sqlite.sqlite);
	CHECK_EQ_STR(sqlite.sqlitePath, "custom.sqlite3");
	CHECK_EQ_INT(sqlite.port, 3306);

	// The port is clamped like the original's std::max(1, std::min(65535, ...)).
	constexpr std::string_view kBadPort = R"("D" { "admin_system" { "port" "70000" } })";
	CHECK_EQ_INT(ParseDatabaseSettings(kBadPort).port, 65535);
	constexpr std::string_view kZeroPort = R"("D" { "admin_system" { "port" "0" } })";
	CHECK_EQ_INT(ParseDatabaseSettings(kZeroPort).port, 1);
}

void TestPhrasesAndLanguage() {
	constexpr std::string_view kPhrases = R"(
"Phrases"
{
	"MainTitle"
	{
		"ru"	"Админ панель"
		"en"	"Admin panel"
	}
	"OnlyRu"
	{
		"ru"	"Только русский"
	}
	"OnlyUa"
	{
		"ua"	"Тільки українська"
	}
}
)";
	const auto ru = ParsePhrases(kPhrases, "ru");
	CHECK(ru.has_value());
	if (ru) {
		CHECK_EQ_STR(ru->at("MainTitle"), "Админ панель");
		CHECK_EQ_STR(ru->at("OnlyRu"), "Только русский");
		// No "ru" entry: the "en" fallback, then the first language the key has.
		CHECK_EQ_STR(ru->at("OnlyUa"), "Тільки українська");
	}
	const auto en = ParsePhrases(kPhrases, "en");
	CHECK(en.has_value());
	if (en) {
		CHECK_EQ_STR(en->at("MainTitle"), "Admin panel");
		// "OnlyRu" has no "en": the first available language is used, never an empty line.
		CHECK_EQ_STR(en->at("OnlyRu"), "Только русский");
	}
	// The pisexcompat fallback language is "en".
	CHECK_EQ_STR(std::string(kFallbackLanguage), "en");

	CHECK(!ParsePhrases("\"Phrases\" {", "ru").has_value());

	// NormalizeLanguage / PickLanguage: core.cfg first, then s2sdk, then "en".
	CHECK_EQ_STR(NormalizeLanguage("russian"), "ru");
	CHECK_EQ_STR(NormalizeLanguage("Russian"), "ru");
	CHECK_EQ_STR(NormalizeLanguage("schinese"), "zh-CN");
	CHECK_EQ_STR(NormalizeLanguage("klingon"), "klingon");
	CHECK_EQ_STR(PickLanguage("russian", "en"), "ru");
	CHECK_EQ_STR(PickLanguage("", "english"), "en");
	CHECK_EQ_STR(PickLanguage("", ""), "en");
}

void TestCandidatesAndPaths() {
	// The exact pisexcompat ResolvePath() order (see admin_config.hpp).
	const std::vector<std::string> core = ConfigCandidates("/cfg", "/data", "core.ini");
	CHECK_EQ_INT(core.size(), 4);
	CHECK_EQ_STR(core[0], "addons/configs/admin_system/core.ini");
	CHECK_EQ_STR(core[1], "/cfg/admin_system/core.ini");
	CHECK_EQ_STR(core[2], "/cfg/admin_system/translations/core.ini");
	CHECK_EQ_STR(core[3], "/data/admin_system/core.ini");

	const std::vector<std::string> databases = DatabaseCandidates("/cfg", "/data");
	CHECK_EQ_INT(databases.size(), 4);
	CHECK_EQ_STR(databases[0], "addons/configs/databases.cfg");
	CHECK_EQ_STR(databases[1], "/cfg/admin_system/databases.cfg");
	CHECK_EQ_STR(databases[2], "/cfg/admin_system/translations/databases.cfg");
	CHECK_EQ_STR(databases[3], "/data/admin_system/databases.cfg");

	const std::vector<std::string> phrases = PhraseCandidates("/cfg");
	CHECK_EQ_INT(phrases.size(), 3);
	CHECK_EQ_STR(phrases[0], "admin_system.phrases.txt");
	CHECK_EQ_STR(phrases[1], "/cfg/admin_system/translations/admin_system.phrases.txt");
	CHECK_EQ_STR(phrases[2], "/cfg/admin_system/admin_system.phrases.txt");

	CHECK_EQ_STR(JoinPath("/cfg", "x.ini"), "/cfg/x.ini");
	CHECK_EQ_STR(JoinPath("/cfg/", "x.ini"), "/cfg/x.ini");
	CHECK_EQ_STR(JoinPath("", "x.ini"), "x.ini");
	CHECK_EQ_STR(ResolveDataFilePath("/data", "admin_system.sqlite3"), "/data/admin_system/admin_system.sqlite3");

	// The SQLite file: relative paths go under <data>/admin_system, absolute paths and ":memory:"
	// are used as given (the parent directory of a relative path is created).
	CHECK_EQ_STR(ResolveSqlitePath("/data", "admin_system.sqlite3"),
				 "/data/admin_system/admin_system.sqlite3");
	CHECK_EQ_STR(ResolveSqlitePath("/data", "/var/lib/db.sqlite3"), "/var/lib/db.sqlite3");
	CHECK_EQ_STR(ResolveSqlitePath("/data", ":memory:"), ":memory:");

	// The candidate order is authoritative: the first existing file wins, even when the later ones
	// would be valid (the Pisex loader's FirstExisting behaviour).
	TempDir dir;
	const std::string configs = dir.Write("cfg/admin_system/core.ini", "\"Config\" { \"database_prefix\" \"first_\" }");
	const std::string raw = dir.Write("addons/configs/admin_system/core.ini",
									 "\"Config\" { \"database_prefix\" \"raw_\" }");
	const std::vector<std::string> ordered{raw, configs};
	Config loaded;
	std::string usedPath;
	CHECK(LoadCoreConfigFile(ordered, loaded, usedPath));
	CHECK_EQ_STR(usedPath, raw);
	CHECK_EQ_STR(loaded.database_prefix, "raw_");

	// The first existing file is authoritative even when it is malformed: no fall-through to a
	// decoy (so a broken production file cannot silently switch the plugin to another config).
	const std::string broken = dir.Write("cfg/broken/core.ini", "\"Config\" {");
	const std::string decoy = dir.Write("cfg/decoy/core.ini", "\"Config\" { \"database_prefix\" \"decoy_\" }");
	Config notLoaded;
	std::string brokenUsedPath;
	CHECK(!LoadCoreConfigFile({broken, decoy}, notLoaded, brokenUsedPath));
	CHECK_EQ_STR(brokenUsedPath, broken);
	CHECK_EQ_STR(notLoaded.database_prefix, "as_");

	// Language: <configs>/core.cfg "ServerLang", nothing else.
	CHECK_EQ_STR(CoreConfigLanguage("/nonexistent-dir"), "");
	const std::string coreCfg = dir.Write("cfg2/core.cfg", "\"Core\" { \"ServerLang\" \"ru\" }");
	(void)coreCfg;
	CHECK_EQ_STR(CoreConfigLanguage(dir.path.string() + "/cfg2"), "ru");
	CHECK_EQ_STR(PickLanguage(CoreConfigLanguage(dir.path.string() + "/cfg2"), "en"), "ru");

	// Phrases: the <configs>/admin_system/translations file is found through the candidate list.
	dir.Write("cfg/admin_system/translations/admin_system.phrases.txt",
			  "\"Phrases\" { \"MainTitle\" { \"ru\" \"Админ панель\" } }");
	std::string phrasesPath;
	const auto phrasesMap = LoadFirstPhrases(PhraseCandidates(dir.path.string() + "/cfg"), "ru", phrasesPath);
	CHECK(phrasesMap.has_value());
	if (phrasesMap) {
		CHECK_EQ_STR(phrasesMap->at("MainTitle"), "Админ панель");
	}
	CHECK_EQ_STR(phrasesPath, dir.path.string() + "/cfg/admin_system/translations/admin_system.phrases.txt");
}

void TestCommandArgs() {
	// CCommand tokenizer: whitespace separated, quotes group and are removed.
	CommandArgs args("!ban 3 60 \"cheating and rude\"");
	CHECK_EQ_INT(args.ArgC(), 4);
	CHECK_EQ_STR(args.Arg(0), "!ban");
	CHECK_EQ_STR(args.Arg(1), "3");
	CHECK_EQ_STR(args.Arg(2), "60");
	CHECK_EQ_STR(args.Arg(3), "cheating and rude");
	CHECK(args.Arg(4) == nullptr);
	CHECK(args.Arg(-1) == nullptr);
	CHECK_EQ_STR(args.ArgS(), "!ban 3 60 \"cheating and rude\"");

	// An unterminated quote runs to the end of the string (CCommand behaviour).
	CommandArgs unterminated("\"a b");
	CHECK_EQ_INT(unterminated.ArgC(), 1);
	CHECK_EQ_STR(unterminated.Arg(0), "a b");

	CommandArgs empty("");
	CHECK_EQ_INT(empty.ArgC(), 0);

	// BuildCommandContent rebuilds the content from s2sdk's tokenized arguments: the registered
	// spelling first, a token with whitespace quoted again so Tokenize() round-trips it.
	plg::vector<plg::string> tokens;
	tokens.emplace_back("ban");
	tokens.emplace_back("John Doe");
	tokens.emplace_back("60");
	const std::string content = BuildCommandContent("!ban", tokens);
	CHECK_EQ_STR(content, "!ban \"John Doe\" 60");
	CommandArgs roundTrip(content.c_str());
	CHECK_EQ_INT(roundTrip.ArgC(), 3);
	CHECK_EQ_STR(roundTrip.Arg(0), "!ban");
	CHECK_EQ_STR(roundTrip.Arg(1), "John Doe");
	CHECK_EQ_STR(roundTrip.Arg(2), "60");

	plg::vector<plg::string> trickyTokens;
	trickyTokens.emplace_back("mm_ban");
	trickyTokens.emplace_back("3");
	// A token that itself contains a double quote is left alone (an escape the engine's tokenizer
	// does not understand would corrupt it differently).
	trickyTokens.emplace_back("he said \"hi\"");
	const std::string trickyContent = BuildCommandContent("mm_ban", trickyTokens);
	CHECK_EQ_STR(trickyContent, "mm_ban 3 he said \"hi\"");
}

} // namespace

int main() {
	TestKv1();
	TestCoreIni();
	TestSorting();
	TestDatabaseSettings();
	TestPhrasesAndLanguage();
	TestCandidatesAndPaths();
	TestCommandArgs();

	if (g_failures != 0) {
		std::printf("admin_config_tests: %d/%d checks failed\n", g_failures, g_checks);
		return 1;
	}
	std::printf("admin_config_tests: all %d checks passed\n", g_checks);
	return 0;
}
