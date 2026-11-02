// Temp directories and configuration files for the admin_system runtime tests (tests only).
//
// The tests drive the real plugin with the plugify directory functions pointed at a temporary tree:
//
//   <temp>/configs/admin_system/core.ini                                   the flags of the test
//   <temp>/configs/admin_system/translations/admin_system.phrases.txt      the phrase of the test
//   <temp>/data/admin_system/admin_system.sqlite3                          the fail-soft database
//   <temp>/work                                                            the working directory
//
// so nothing is written into the source tree and the candidate order of the plugin
// (src/admin_config.hpp) is exercised for real.

#pragma once

#include <cstdint>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <string>
#include <string_view>

#include <plg/plugin.hpp>
#include <plg/string.hpp>

namespace admin_test {

inline std::string g_configsPath;
inline std::string g_dataPath;

struct TempDirs {
	std::filesystem::path previous;
	std::filesystem::path root;

	TempDirs() {
		static int counter = 0;
		previous = std::filesystem::current_path();
		root = std::filesystem::temp_directory_path() /
			   ("admin_system_commands_tests_" + std::to_string(::getpid()) + "_" + std::to_string(++counter));
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		std::filesystem::create_directories(root / "configs" / "admin_system" / "translations", ec);
		std::filesystem::create_directories(root / "data" / "admin_system", ec);
		std::filesystem::create_directories(root / "work", ec);
		std::filesystem::current_path(root / "work", ec);
		g_configsPath = (root / "configs").string();
		g_dataPath = (root / "data").string();
	}
	~TempDirs() {
		std::error_code ec;
		std::filesystem::current_path(previous, ec);
		std::filesystem::remove_all(root, ec);
		g_configsPath.clear();
		g_dataPath.clear();
	}
	TempDirs(const TempDirs&) = delete;
	TempDirs& operator=(const TempDirs&) = delete;
};

inline plg::string FakeConfigsDir() {
	return plg::string(g_configsPath);
}
inline plg::string FakeDataDir() {
	return plg::string(g_dataPath);
}

inline void WriteFile(const std::filesystem::path& path, std::string_view content) {
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary);
	out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

// core.ini: only the flag table matters for the tests - flag "b" grants the punish and the group
// permissions the original maps it to. Every other key keeps the plugin's built-in default.
inline constexpr std::string_view kTestCoreIni = R"KV(
"Config"
{
    "database_prefix" "as_"
    "flags"
    {
        "b"
        {
            "name" "Ban"
            "access"
            {
                "@admin/ban"          "1"
                "@admin/mute"         "1"
                "@admin/add_group"    "1"
                "@admin/remove_group" "1"
            }
        }
    }
}
)KV";

// admin_system.phrases.txt: GroupAdded is the only key the command replies use with an argument.
inline constexpr std::string_view kTestPhrases = R"KV(
"Phrases"
{
    "GroupAdded"
    {
        "en" "Group added (ID: %d)"
    }
    "PlayerNotFound"
    {
        "en" "Player not found"
    }
    "UsagePunish"
    {
        "en" "Usage: %s <userid|steamid> <time> <reason>"
    }
}
)KV";

inline void WriteAdminConfigs(const TempDirs& dirs) {
	WriteFile(dirs.root / "configs" / "admin_system" / "core.ini", kTestCoreIni);
	WriteFile(dirs.root / "configs" / "admin_system" / "translations" / "admin_system.phrases.txt", kTestPhrases);
}

} // namespace admin_test
