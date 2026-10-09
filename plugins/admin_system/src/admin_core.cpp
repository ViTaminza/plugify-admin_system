#include "admin_core.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace admin_system {

namespace {

// --- small helpers -------------------------------------------------------------

// The original's OnlyDigits() accepted an empty string and then called std::stoll on it, which
// threw; an empty or non-numeric value is simply "not a steamid" here.
bool OnlyDigits(std::string_view text) {
	if (text.empty()) {
		return false;
	}
	return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

uint64_t ParseU64(std::string_view text) {
	if (!OnlyDigits(text)) {
		return 0;
	}
	try {
		return std::stoull(std::string(text));
	} catch (const std::exception&) {
		return 0;
	}
}

std::string SteamIdText(uint64_t steamid) {
	return std::to_string(steamid);
}

// Table names cannot be bound as parameters, so the prefix - which SetConfig() validated - is
// substituted into the "{p}" placeholders of every statement.
std::string WithPrefix(std::string_view sql, std::string_view prefix) {
	std::string result;
	result.reserve(sql.size() + prefix.size());
	for (size_t i = 0; i < sql.size(); ++i) {
		if (sql[i] == '{' && i + 2 < sql.size() && sql[i + 1] == 'p' && sql[i + 2] == '}') {
			result.append(prefix);
			i += 2;
		} else {
			result.push_back(sql[i]);
		}
	}
	return result;
}

void AppendFlagsAndPermissions(AdminRecord& admin, std::string_view flags, const Config& config) {
	for (const char c : flags) {
		const std::string flag(1, c);
		const std::vector<std::string> permissions = config.PermissionsByFlag(flag);
		admin.permissions.insert(admin.permissions.end(), permissions.begin(), permissions.end());
		admin.flags.push_back(flag);
	}
}

std::string BuildAdminSelect() {
	return "SELECT a.id, a.name, s.flags, s.immunity, s.expires, s.group_id, g.name AS group_name, "
		   "g.flags AS group_flags, g.immunity AS group_immunity, s.server_id "
		   "FROM {p}admins a LEFT JOIN {p}admins_servers s ON a.id = s.admin_id "
		   "LEFT JOIN {p}groups g ON s.group_id = g.id";
}

std::string BuildPunishmentSelect() {
	return "SELECT p.id, p.name, p.steamid, p.ip, p.admin_id, p.created, p.expires, p.reason, "
		   "p.unpunish_admin_id, p.server_id, p.punish_type, "
		   "a.steamid AS admin_steamid, a.name AS admin_name, a.id AS admin_db_id "
		   "FROM {p}punishments p LEFT JOIN {p}admins a ON p.admin_id = a.id";
}

// Column lookup: by name when the driver reports it (the ported SELECTs alias everything),
// falling back to the position of the column in the SELECT list.
size_t Column(const plgdb::Result& result, std::string_view name, size_t fallback) {
	if (const std::optional<size_t> index = result.ColumnIndex(name)) {
		return *index;
	}
	return fallback;
}

std::optional<std::string> OptionalString(const plgdb::Result& result, size_t row, size_t column) {
	if (result.IsNull(row, column)) {
		return std::nullopt;
	}
	return result.GetString(row, column);
}

std::optional<int> OptionalInt(const plgdb::Result& result, size_t row, size_t column) {
	if (result.IsNull(row, column)) {
		return std::nullopt;
	}
	return static_cast<int>(result.GetInt(row, column));
}

std::optional<int64_t> OptionalInt64(const plgdb::Result& result, size_t row, size_t column) {
	if (result.IsNull(row, column)) {
		return std::nullopt;
	}
	return result.GetInt(row, column);
}

std::vector<AdminRow> ParseAdminRows(const plgdb::Result& result) {
	std::vector<AdminRow> rows;
	rows.reserve(result.RowCount());
	const size_t c_id = Column(result, "id", 0);
	const size_t c_name = Column(result, "name", 1);
	const size_t c_flags = Column(result, "flags", 2);
	const size_t c_immunity = Column(result, "immunity", 3);
	const size_t c_expires = Column(result, "expires", 4);
	const size_t c_group_id = Column(result, "group_id", 5);
	const size_t c_group_name = Column(result, "group_name", 6);
	const size_t c_group_flags = Column(result, "group_flags", 7);
	const size_t c_group_immunity = Column(result, "group_immunity", 8);
	const size_t c_server_id = Column(result, "server_id", 9);
	for (size_t row = 0; row < result.RowCount(); ++row) {
		AdminRow parsed;
		parsed.admin_id = result.GetInt(row, c_id, 0);
		parsed.name = OptionalString(result, row, c_name);
		parsed.flags = OptionalString(result, row, c_flags);
		parsed.immunity = OptionalInt(result, row, c_immunity);
		parsed.expires = result.GetInt(row, c_expires, 0);
		parsed.group_id = OptionalInt64(result, row, c_group_id);
		parsed.group_name = OptionalString(result, row, c_group_name);
		parsed.group_flags = OptionalString(result, row, c_group_flags);
		parsed.group_immunity = OptionalInt(result, row, c_group_immunity);
		parsed.server_id = result.GetInt(row, c_server_id, 0);
		rows.push_back(std::move(parsed));
	}
	return rows;
}

std::vector<PunishmentRow> ParsePunishmentRows(const plgdb::Result& result) {
	std::vector<PunishmentRow> rows;
	rows.reserve(result.RowCount());
	const size_t c_id = Column(result, "id", 0);
	const size_t c_name = Column(result, "name", 1);
	const size_t c_steamid = Column(result, "steamid", 2);
	const size_t c_ip = Column(result, "ip", 3);
	const size_t c_admin_id = Column(result, "admin_id", 4);
	const size_t c_created = Column(result, "created", 5);
	const size_t c_expires = Column(result, "expires", 6);
	const size_t c_reason = Column(result, "reason", 7);
	const size_t c_unpunish = Column(result, "unpunish_admin_id", 8);
	const size_t c_server_id = Column(result, "server_id", 9);
	const size_t c_type = Column(result, "punish_type", 10);
	const size_t c_admin_steamid = Column(result, "admin_steamid", 11);
	const size_t c_admin_name = Column(result, "admin_name", 12);
	const size_t c_admin_db_id = Column(result, "admin_db_id", 13);
	for (size_t row = 0; row < result.RowCount(); ++row) {
		PunishmentRow parsed;
		parsed.id = result.GetInt(row, c_id, 0);
		parsed.name = OptionalString(result, row, c_name);
		parsed.steamid = result.GetString(row, c_steamid);
		parsed.ip = result.GetString(row, c_ip);
		parsed.admin_id = result.GetInt(row, c_admin_id, 0);
		parsed.created = result.GetInt(row, c_created, 0);
		parsed.expires = result.GetInt(row, c_expires, 0);
		parsed.reason = result.GetString(row, c_reason);
		parsed.unpunish_null = result.IsNull(row, c_unpunish);
		parsed.server_id = result.GetInt(row, c_server_id, 0);
		parsed.punish_type = result.GetInt(row, c_type, 0);
		parsed.admin_steamid = OptionalString(result, row, c_admin_steamid);
		parsed.admin_name = OptionalString(result, row, c_admin_name);
		parsed.admin_db_id = result.GetInt(row, c_admin_db_id, 0);
		rows.push_back(std::move(parsed));
	}
	return rows;
}

// --- schema ---------------------------------------------------------------------
// Byte for byte the CREATE TABLE statements of database.cpp, with the prefix of core.ini; only
// the auto increment spelling differs between the drivers.
struct DdlStatement {
	std::string_view mysql;
	std::string_view sqlite;
};

const DdlStatement kCreateGroups{
	"CREATE TABLE IF NOT EXISTS {p}groups ("
	"id INT PRIMARY KEY AUTO_INCREMENT,"
	"name VARCHAR(128),"
	"flags VARCHAR(64),"
	"immunity INT,"
	"UNIQUE (name)"
	");",
	"CREATE TABLE IF NOT EXISTS {p}groups ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"name VARCHAR(128),"
	"flags VARCHAR(64),"
	"immunity INT,"
	"UNIQUE (name)"
	");"};

const DdlStatement kCreateAdmins{
	"CREATE TABLE IF NOT EXISTS {p}admins ("
	"id INT PRIMARY KEY AUTO_INCREMENT,"
	"name VARCHAR(256),"
	"steamid VARCHAR(32),"
	"comment VARCHAR(256) DEFAULT NULL"
	");",
	"CREATE TABLE IF NOT EXISTS {p}admins ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"name VARCHAR(256),"
	"steamid VARCHAR(32),"
	"comment VARCHAR(256) DEFAULT NULL"
	");"};

const DdlStatement kCreateAdminsServers{
	"CREATE TABLE IF NOT EXISTS {p}admins_servers ("
	"id INT PRIMARY KEY AUTO_INCREMENT,"
	"admin_id INT,"
	"flags VARCHAR(64) DEFAULT NULL,"
	"immunity INT DEFAULT 0,"
	"expires INT DEFAULT 0,"
	"group_id INT DEFAULT NULL,"
	"server_id INT,"
	"UNIQUE (admin_id, server_id)"
	");",
	"CREATE TABLE IF NOT EXISTS {p}admins_servers ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"admin_id INT,"
	"flags VARCHAR(64) DEFAULT NULL,"
	"immunity INT DEFAULT 0,"
	"expires INT DEFAULT 0,"
	"group_id INT DEFAULT NULL,"
	"server_id INT,"
	"UNIQUE (admin_id, server_id)"
	");"};

const DdlStatement kCreatePunishments{
	"CREATE TABLE IF NOT EXISTS {p}punishments ("
	"id INT PRIMARY KEY AUTO_INCREMENT,"
	"name VARCHAR(256),"
	"steamid VARCHAR(32),"
	"ip VARCHAR(32),"
	"admin_id INT,"
	"created INT,"
	"expires INT,"
	"reason VARCHAR(256),"
	"unpunish_admin_id INT DEFAULT NULL,"
	"server_id INT,"
	"punish_type INT"
	");",
	"CREATE TABLE IF NOT EXISTS {p}punishments ("
	"id INTEGER PRIMARY KEY AUTOINCREMENT,"
	"name VARCHAR(256),"
	"steamid VARCHAR(32),"
	"ip VARCHAR(32),"
	"admin_id INT,"
	"created INT,"
	"expires INT,"
	"reason VARCHAR(256),"
	"unpunish_admin_id INT DEFAULT NULL,"
	"server_id INT,"
	"punish_type INT"
	");"};

const DdlStatement* const kCreateTables[] = {&kCreateGroups, &kCreateAdmins, &kCreateAdminsServers, &kCreatePunishments};

std::string_view PickDdl(const DdlStatement& statement, plgdb::Driver driver) {
	return driver == plgdb::Driver::MySQL ? statement.mysql : statement.sqlite;
}

} // namespace

// --- config -----------------------------------------------------------------------

bool Config::HasValidPrefix() const {
	if (database_prefix.empty() || database_prefix.size() > 32) {
		return false;
	}
	return std::all_of(database_prefix.begin(), database_prefix.end(), [](unsigned char c) {
		return std::isalnum(c) != 0 || c == '_';
	});
}

const Flag* Config::FindFlag(std::string_view flag) const {
	const auto it = flags.find(flag);
	return it == flags.end() ? nullptr : &it->second;
}

std::vector<std::string> Config::PermissionsByFlag(std::string_view flag) const {
	const Flag* found = FindFlag(flag);
	return found ? found->permissions : std::vector<std::string>{};
}

std::string Config::FlagName(std::string_view flag) const {
	const Flag* found = FindFlag(flag);
	return found ? found->name : std::string();
}

bool Config::IsDefaultPermission(std::string_view permission) const {
	return std::find(default_permissions.begin(), default_permissions.end(), permission) != default_permissions.end();
}

// --- pure computations ---------------------------------------------------------------

int64_t MergeExpiry(int64_t current, int64_t incoming) {
	// PARITY: exactly the original's "if (current != 0 && current < incoming) current = incoming;"
	if (current == kPermanent) {
		return current;
	}
	return current < incoming ? incoming : current;
}

AdminRecord ComputeAdminRecord(const std::vector<AdminRow>& rows, const Config& config, std::string_view player_name) {
	AdminRecord admin;
	admin.expire_time = kNoExpiry;
	for (const AdminRow& row : rows) {
		admin.id = row.admin_id;
		admin.name = row.name ? *row.name : std::string(player_name);

		// PARITY: the server immunity is folded in before the server filter, the group immunity
		// after it - the original applied them in that order.
		if (const int immunity = row.immunity.value_or(0); immunity > admin.immunity) {
			admin.immunity = immunity;
		}
		const int group_immunity = row.group_immunity.value_or(0);
		if (config.admin_server_id != -1 && row.server_id != config.admin_server_id && row.server_id != -1) {
			continue;
		}
		if (group_immunity > admin.immunity) {
			admin.immunity = group_immunity;
		}
		if ((row.expires > admin.expire_time || row.expires == 0) && admin.expire_time != 0) {
			admin.expire_time = row.expires;
		}
		if (row.flags) {
			AppendFlagsAndPermissions(admin, *row.flags, config);
		}
		if (row.group_id && *row.group_id != 0) {
			admin.group_id = *row.group_id;
			admin.group_name = row.group_name.value_or("");
			AppendFlagsAndPermissions(admin, row.group_flags.value_or(""), config);
		}
	}
	return admin;
}

PunishmentLoadOutcome ApplyPunishmentRows(const std::vector<PunishmentRow>& rows, const Config& config, int64_t now,
										  bool forced, std::array<Punishment, kPunishmentTypes>& out) {
	PunishmentLoadOutcome outcome;
	outcome.forced = forced;
	for (const PunishmentRow& row : rows) {
		outcome.found = true;
		if (config.punish_server_id != -1 && row.server_id != config.punish_server_id && row.server_id != -1) {
			continue;
		}
		const int type = static_cast<int>(row.punish_type);
		if (type < 0 || type >= kPunishmentTypes) {
			continue;
		}
		if (row.expires != 0 && row.expires < now) {
			continue;
		}
		if (type == RT_BAN) {
			outcome.ban = true;
			if (!forced) {
				// PARITY: the connect path leaves the loop at the first ban.
				break;
			}
			continue;
		}
		Punishment& punishment = out[static_cast<size_t>(type)];
		punishment.expires = MergeExpiry(punishment.expires, row.expires);
		punishment.reason = row.reason;
		if (row.admin_steamid) {
			if (const uint64_t steamid = ParseU64(*row.admin_steamid); steamid != 0) {
				punishment.admin_steamid = steamid;
			}
		}
	}
	return outcome;
}

// --- core ---------------------------------------------------------------------------

AdminCore::AdminCore(plgdb::Database& db) : m_db(db), m_life(std::make_shared<int>(0)) {
	m_clock = [] { return static_cast<int64_t>(std::time(nullptr)); };
	// PARITY (fix): the original's arrays started at 0, which reported "punished" before the
	// first load; -1 (Punishment{}) is the value its own ResetPunishments() used.
}

AdminCore::~AdminCore() {
	// Callbacks still queued in plgdb find an expired token and do nothing.
	m_life.reset();
}

void AdminCore::SetConfig(Config config) {
	if (!config.database_prefix.empty() && !config.HasValidPrefix()) {
		ReportError("admin_system: database_prefix '" + config.database_prefix + "' is not a plain identifier, keeping '" +
					m_config.database_prefix + "'");
		config.database_prefix = m_config.database_prefix;
	}
	m_config = std::move(config);
}

void AdminCore::SetClock(std::function<int64_t()> clock) {
	m_clock = clock ? std::move(clock) : std::function<int64_t()>([] { return static_cast<int64_t>(std::time(nullptr)); });
}

int64_t AdminCore::Now() const {
	return m_clock();
}

std::string AdminCore::TableGroups() const {
	return m_config.database_prefix + "groups";
}

std::string AdminCore::TableAdmins() const {
	return m_config.database_prefix + "admins";
}

std::string AdminCore::TableAdminsServers() const {
	return m_config.database_prefix + "admins_servers";
}

std::string AdminCore::TablePunishments() const {
	return m_config.database_prefix + "punishments";
}

void AdminCore::ReportError(std::string_view message) const {
	if (onError) {
		onError(message);
	}
}

bool AdminCore::SlotInRange(int slot) const {
	return slot >= 0 && slot < kMaxPlayers;
}

AdminCore::SlotState* AdminCore::FindSlot(int slot) {
	return SlotInRange(slot) ? &m_slots[static_cast<size_t>(slot)] : nullptr;
}

const AdminCore::SlotState* AdminCore::FindSlot(int slot) const {
	return SlotInRange(slot) ? &m_slots[static_cast<size_t>(slot)] : nullptr;
}

Punishment* AdminCore::FindPunishment(int slot, int type) {
	if (!SlotInRange(slot) || type < 0 || type >= kPunishmentTypes) {
		return nullptr;
	}
	return &m_punishments[static_cast<size_t>(slot)][static_cast<size_t>(type)];
}

const Punishment* AdminCore::FindPunishment(int slot, int type) const {
	if (!SlotInRange(slot) || type < 0 || type >= kPunishmentTypes) {
		return nullptr;
	}
	return &m_punishments[static_cast<size_t>(slot)][static_cast<size_t>(type)];
}

void AdminCore::Query(std::string sql, plgdb::Params params, plgdb::Callback callback) {
	// Every core callback goes through here, so one lifetime token is enough: a result that
	// lands after the core is gone is dropped instead of touching freed state.
	std::weak_ptr<void> token = m_life;
	m_db.Query(WithPrefix(sql, m_config.database_prefix), std::move(params),
			   [token, callback = std::move(callback)](const plgdb::Result& result) {
				   if (token.expired() || !callback) {
					   return;
				   }
				   callback(result);
			   });
}

// --- schema ------------------------------------------------------------------------

void AdminCore::CreateSchema() {
	for (const DdlStatement* statement : kCreateTables) {
		const plgdb::Result result = m_db.QuerySync(WithPrefix(PickDdl(*statement, m_db.GetDriver()), m_config.database_prefix), {});
		if (!result.Ok()) {
			throw std::runtime_error("admin_system: creating the " + m_config.database_prefix + "* tables failed: " + result.Error());
		}
	}

	// The reserved console row (id 1) is what admin_id 1 in the punishment table refers to.
	const std::string select = WithPrefix("SELECT id FROM {p}admins WHERE steamid = ?", m_config.database_prefix);
	const plgdb::Result existing = m_db.QuerySync(select, {std::string(kConsoleSteamId)});
	if (!existing.Ok()) {
		throw std::runtime_error("admin_system: reading the console admin row failed: " + existing.Error());
	}
	if (existing.RowCount() == 0) {
		const plgdb::Result inserted = m_db.QuerySync(
			WithPrefix("INSERT INTO {p}admins (id, name, steamid) VALUES (1, 'Console', '0')", m_config.database_prefix), {});
		if (!inserted.Ok()) {
			throw std::runtime_error("admin_system: creating the console admin row failed: " + inserted.Error());
		}
	}
}

// --- admin state ---------------------------------------------------------------------

const AdminRecord* AdminCore::FindAdmin(int slot) const {
	return SlotInRange(slot) ? &m_admins[static_cast<size_t>(slot)] : nullptr;
}

bool AdminCore::HasPermission(int slot, std::string_view permission) const {
	if (slot == -1) {
		return true; // PARITY: console
	}
	if (!SlotInRange(slot) || permission.empty()) {
		return false;
	}
	if (m_config.IsDefaultPermission(permission)) {
		return true;
	}
	// One pass: the permission itself, plus the "@admin/root" wildcard (a second std::find over
	// the same list only doubles the cost of the calls other plugins make from commands/menus/timers).
	const AdminRecord& admin = m_admins[static_cast<size_t>(slot)];
	bool hasRoot = false;
	for (const std::string& entry : admin.permissions) {
		if (entry == permission) {
			return true;
		}
		if (entry == "@admin/root") {
			hasRoot = true;
		}
	}
	return hasRoot;
}

bool AdminCore::HasFlag(int slot, std::string_view flag) const {
	if (slot == -1) {
		return true;
	}
	if (!SlotInRange(slot) || flag.empty()) {
		return false;
	}
	const std::vector<std::string>& flags = m_admins[static_cast<size_t>(slot)].flags;
	return std::find(flags.begin(), flags.end(), flag) != flags.end();
}

bool AdminCore::IsAdmin(int slot) const {
	if (slot == -1) {
		return true;
	}
	if (!SlotInRange(slot)) {
		return false;
	}
	return m_admins[static_cast<size_t>(slot)].IsAdmin();
}

void AdminCore::AddLocalFlag(int slot, std::string_view flag) {
	if (SlotInRange(slot) && !flag.empty()) {
		m_admins[static_cast<size_t>(slot)].flags.emplace_back(flag);
	}
}

void AdminCore::RemoveLocalFlag(int slot, std::string_view flag) {
	if (!SlotInRange(slot)) {
		return;
	}
	std::vector<std::string>& flags = m_admins[static_cast<size_t>(slot)].flags;
	flags.erase(std::remove(flags.begin(), flags.end(), flag), flags.end());
}

void AdminCore::AddLocalPermission(int slot, std::string_view permission) {
	if (SlotInRange(slot) && !permission.empty()) {
		m_admins[static_cast<size_t>(slot)].permissions.emplace_back(permission);
	}
}

void AdminCore::RemoveLocalPermission(int slot, std::string_view permission) {
	if (!SlotInRange(slot)) {
		return;
	}
	std::vector<std::string>& permissions = m_admins[static_cast<size_t>(slot)].permissions;
	permissions.erase(std::remove(permissions.begin(), permissions.end(), permission), permissions.end());
}

void AdminCore::AddLocalImmunity(int slot, int immunity) {
	if (SlotInRange(slot)) {
		m_admins[static_cast<size_t>(slot)].immunity = immunity;
	}
}

void AdminCore::RemoveLocalImmunity(int slot) {
	if (SlotInRange(slot)) {
		m_admins[static_cast<size_t>(slot)].immunity = 0;
	}
}

void AdminCore::ResetPunishments(int slot) {
	if (!SlotInRange(slot)) {
		return;
	}
	for (int type = 0; type < kPunishmentTypes; ++type) {
		m_punishments[static_cast<size_t>(slot)][static_cast<size_t>(type)] = Punishment{};
	}
}

void AdminCore::ResetSlot(int slot) {
	SlotState* state = FindSlot(slot);
	if (state == nullptr) {
		return;
	}
	++state->generation;
	ResetPunishments(slot);
	m_admins[static_cast<size_t>(slot)].Clear();
}

int64_t AdminCore::AdminDbId(int slot) const {
	// The original used 1 (the console row) for admin_id when there was no admin.
	return slot == -1 ? 1 : m_admins[static_cast<size_t>(slot)].id;
}

uint64_t AdminCore::AdminSteamId(int slot) const {
	return (slot == -1 || !SlotInRange(slot)) ? 0 : m_admins[static_cast<size_t>(slot)].steamid;
}

// --- admin loading --------------------------------------------------------------------

void AdminCore::LoadAdmin(int slot, uint64_t steamid, std::string player_name) {
	SlotState* state = FindSlot(slot);
	if (state == nullptr) {
		return;
	}
	// PARITY: CheckPermissions cleared the slot first, so a load that finds nothing leaves an
	// empty (non-admin) record behind.
	m_admins[static_cast<size_t>(slot)].Clear();
	m_admins[static_cast<size_t>(slot)].steamid = steamid;
	const uint64_t generation = state->generation;

	Query(BuildAdminSelect() + " WHERE a.steamid = ?", {SteamIdText(steamid)},
		  [this, slot, generation, steamid, player_name = std::move(player_name)](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: loading the admin of " + SteamIdText(steamid) + " failed: " + result.Error());
				  return;
			  }
			  const SlotState* current = FindSlot(slot);
			  if (current == nullptr || current->generation != generation) {
				  return; // the slot was reset (or reused) while the query was in flight
			  }
			  if (result.RowCount() == 0) {
				  return; // PARITY: no row, no OnAdminConnected
			  }
			  AdminRecord record = ComputeAdminRecord(ParseAdminRows(result), m_config, player_name);
			  record.steamid = steamid;
			  m_admins[static_cast<size_t>(slot)] = std::move(record);
			  if (onAdminLoaded) {
				  onAdminLoaded(slot, m_admins[static_cast<size_t>(slot)]);
			  }
		  });
}

// --- punishments ------------------------------------------------------------------------

bool AdminCore::IsPunished(int slot, int type) const {
	return PunishmentExpires(slot, type) != kNoExpiry;
}

int64_t AdminCore::PunishmentExpires(int slot, int type) const {
	const Punishment* punishment = FindPunishment(slot, type);
	return punishment == nullptr ? kNoExpiry : punishment->expires;
}

std::string_view AdminCore::PunishmentReason(int slot, int type) const {
	const Punishment* punishment = FindPunishment(slot, type);
	return punishment == nullptr ? std::string_view{} : std::string_view(punishment->reason);
}

uint64_t AdminCore::PunishmentAdminSteamId(int slot, int type) const {
	const Punishment* punishment = FindPunishment(slot, type);
	return punishment == nullptr ? 0 : punishment->admin_steamid;
}

bool AdminCore::IsMutedOrSilenced(int slot) const {
	const int64_t now = Now();
	for (const int type : {RT_MUTE, RT_SILENCE}) {
		const Punishment* punishment = FindPunishment(slot, type);
		if (punishment == nullptr || punishment->expires == kNoExpiry) {
			continue;
		}
		if (punishment->expires == kPermanent || punishment->expires > now) {
			return true;
		}
	}
	return false;
}

bool AdminCore::IsGaggedOrSilenced(int slot) const {
	const int64_t now = Now();
	for (const int type : {RT_GAG, RT_SILENCE}) {
		const Punishment* punishment = FindPunishment(slot, type);
		if (punishment == nullptr || punishment->expires == kNoExpiry) {
			continue;
		}
		if (punishment->expires == kPermanent || punishment->expires > now) {
			return true;
		}
	}
	return false;
}

ExpiryOutcome AdminCore::ExpireState() {
	const int64_t now = Now();
	ExpiryOutcome outcome;
	for (int slot = 0; slot < kMaxPlayers; ++slot) {
		// PARITY: the original's timer walked the punishment types 1..3 (bans are never stored).
		for (int type = 1; type < kPunishmentTypes; ++type) {
			Punishment& punishment = m_punishments[static_cast<size_t>(slot)][static_cast<size_t>(type)];
			if (punishment.expires == kNoExpiry || punishment.expires == kPermanent) {
				continue;
			}
			if (punishment.expires < now) {
				punishment = Punishment{};
				outcome.punishments.push_back({slot, type});
			}
		}
		AdminRecord& admin = m_admins[static_cast<size_t>(slot)];
		if (admin.id > 0 && admin.expire_time != kPermanent && admin.expire_time != kNoExpiry && admin.expire_time < now) {
			admin.Clear();
			outcome.admins.push_back(slot);
		}
	}
	return outcome;
}

bool AdminCore::CanPunish(int admin_slot, int admin_immunity, int target_immunity, int immunity_type) {
	if (admin_slot == -1) {
		return true; // PARITY (fix): the original indexed g_pAdmins[-1] for the console
	}
	switch (immunity_type) {
	case 1:
		return admin_immunity >= target_immunity;
	case 2:
		return admin_immunity > target_immunity;
	case 3:
		return target_immunity == 0;
	default:
		return true; // 0 and anything unknown: immunity does not matter
	}
}

bool AdminCore::Punish(const PlayerIdentity& player, int type, int time, std::string reason, int admin_slot) {
	if (!SlotInRange(player.slot) || type < 0 || type >= kPunishmentTypes) {
		return false;
	}
	if (player.steamid == 0) {
		ReportError("admin_system: refusing to punish slot " + std::to_string(player.slot) + " without a steamid");
		return false;
	}
	if (onPlayerPunishPre && onPlayerPunishPre(player.slot, type, time, reason, admin_slot)) {
		return false;
	}

	const int64_t now = Now();
	const int64_t expires = time == 0 ? kPermanent : now + time;
	const std::string ip = m_config.punish_ip ? player.ip : std::string();
	// The slot is that steamid from now on, so Unpunish()/UnpunishOffline() and the admin
	// records can rely on it (the original re-read g_pPlayers->GetSteamID64(slot) each time).
	m_admins[static_cast<size_t>(player.slot)].steamid = player.steamid;

	Query("INSERT INTO {p}punishments (name, steamid, ip, admin_id, created, expires, reason, server_id, punish_type) "
		  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
		  {player.name, SteamIdText(player.steamid), ip, AdminDbId(admin_slot), now, expires, reason, m_config.punish_server_id,
		   static_cast<int64_t>(type)},
		  [this](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: storing a punishment failed: " + result.Error());
			  }
		  });

	Punishment& punishment = m_punishments[static_cast<size_t>(player.slot)][static_cast<size_t>(type)];
	punishment.expires = expires;
	punishment.reason = reason;
	punishment.admin_steamid = AdminSteamId(admin_slot);

	if (onPlayerPunish) {
		onPlayerPunish(player.slot, type, time, punishment.reason, admin_slot);
	}
	return true;
}

bool AdminCore::TryPunish(const PlayerIdentity& player, int type, int time, std::string reason, int admin_slot) {
	const AdminRecord* admin = FindAdmin(admin_slot);
	const AdminRecord* target = FindAdmin(player.slot);
	const int admin_immunity = admin == nullptr ? 0 : admin->immunity;
	const int target_immunity = target == nullptr ? 0 : target->immunity;
	if (!CanPunish(admin_slot, admin_immunity, target_immunity, m_config.immunity_type)) {
		return false;
	}
	return Punish(player, type, time, std::move(reason), admin_slot);
}

void AdminCore::PunishOffline(std::string steamid64, std::string name, int type, int time, std::string reason, int admin_slot) {
	if (steamid64.empty() || type < 0 || type >= kPunishmentTypes) {
		return;
	}
	if (onOfflinePlayerPunishPre && onOfflinePlayerPunishPre(steamid64, name, type, time, reason, admin_slot)) {
		return;
	}
	const int64_t now = Now();
	const int64_t expires = time == 0 ? kPermanent : now + time;
	Query("INSERT INTO {p}punishments (name, steamid, admin_id, created, expires, reason, server_id, punish_type) "
		  "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
		  {name, steamid64, AdminDbId(admin_slot), now, expires, reason, m_config.punish_server_id, static_cast<int64_t>(type)},
		  [this](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: storing an offline punishment failed: " + result.Error());
			  }
		  });
	if (onOfflinePlayerPunish) {
		onOfflinePlayerPunish(steamid64, name, type, time, reason, admin_slot);
	}
}

void AdminCore::TryPunishOffline(std::string steamid64, std::string name, int type, int time, std::string reason, int admin_slot) {
	if (admin_slot == -1 || steamid64.empty()) {
		// PARITY: console (or an unknown caller) skips the immunity query entirely.
		PunishOffline(std::move(steamid64), std::move(name), type, time, std::move(reason), admin_slot);
		return;
	}
	const AdminRecord* admin = FindAdmin(admin_slot);
	const int admin_immunity = admin == nullptr ? 0 : admin->immunity;

	std::string sql = BuildAdminSelect() + " WHERE a.steamid = ?";
	std::vector<plgdb::Value> params{steamid64};
	if (m_config.punish_server_id != -1) {
		// PARITY: the original filtered on the punishment server id here, not the admin one.
		sql += " AND (s.server_id = ? OR s.server_id = -1)";
		params.emplace_back(static_cast<int64_t>(m_config.punish_server_id));
	}
	Query(std::move(sql), std::move(params),
		  [this, steamid64, name, type, time, reason, admin_slot, admin_immunity](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: reading the immunity of " + steamid64 + " failed: " + result.Error());
				  return;
			  }
			  int target_immunity = 0;
			  if (result.RowCount() > 0) {
				  // PARITY: the original only looked at the first row.
				  const std::vector<AdminRow> rows = ParseAdminRows(result);
				  target_immunity = rows.front().immunity.value_or(0);
				  if (const int group_immunity = rows.front().group_immunity.value_or(0); group_immunity > target_immunity) {
					  target_immunity = group_immunity;
				  }
			  }
			  if (!CanPunish(admin_slot, admin_immunity, target_immunity, m_config.immunity_type)) {
				  return;
			  }
			  PunishOffline(steamid64, name, type, time, reason, admin_slot);
		  });
}

// --- loading punishments -------------------------------------------------------------------

namespace {

// The IP of the original: punish_ip decides whether it takes part at all.
std::vector<plgdb::Value> PunishmentQueryParams(const Config& config, uint64_t steamid, const std::string& ip) {
	std::vector<plgdb::Value> params{SteamIdText(steamid)};
	if (config.punish_ip && !ip.empty()) {
		params.emplace_back(ip);
	}
	return params;
}

std::string BuildPunishmentWhere(const Config& config, const std::string& ip) {
	std::string where = " WHERE (p.steamid = ?";
	if (config.punish_ip && !ip.empty()) {
		where += " OR p.ip = ?";
	}
	where += ") AND p.unpunish_admin_id IS NULL";
	return where;
}

} // namespace

void AdminCore::LoadPunishments(int slot, uint64_t steamid, std::string ip) {
	// The plugin calls ResetSlot() when the player connects; keep the steamid around so that
	// Unpunish() finds it.
	if (!SlotInRange(slot)) {
		return;
	}
	m_admins[static_cast<size_t>(slot)].steamid = steamid;
	const SlotState* state = FindSlot(slot);
	const uint64_t generation = state->generation;

	Query(BuildPunishmentSelect() + BuildPunishmentWhere(m_config, ip), PunishmentQueryParams(m_config, steamid, ip),
		  [this, slot, generation, steamid](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: loading the punishments of " + SteamIdText(steamid) + " failed: " + result.Error());
				  return;
			  }
			  const SlotState* current = FindSlot(slot);
			  if (current == nullptr || current->generation != generation) {
				  return;
			  }
			  const std::vector<PunishmentRow> rows = ParsePunishmentRows(result);
			  const PunishmentLoadOutcome outcome =
				  ApplyPunishmentRows(rows, m_config, Now(), /*forced=*/false, m_punishments[static_cast<size_t>(slot)]);
			  if (onPunishmentsLoaded) {
				  onPunishmentsLoaded(slot, outcome);
			  }
		  });
}

void AdminCore::ReloadPunishments(int slot, uint64_t steamid, std::string ip) {
	if (!SlotInRange(slot)) {
		return;
	}
	m_admins[static_cast<size_t>(slot)].steamid = steamid;
	const SlotState* state = FindSlot(slot);
	const uint64_t generation = state->generation;

	Query(BuildPunishmentSelect() + BuildPunishmentWhere(m_config, ip), PunishmentQueryParams(m_config, steamid, ip),
		  [this, slot, generation, steamid](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: reloading the punishments of " + SteamIdText(steamid) + " failed: " + result.Error());
				  return;
			  }
			  const SlotState* current = FindSlot(slot);
			  if (current == nullptr || current->generation != generation) {
				  return;
			  }
			  const std::vector<PunishmentRow> rows = ParsePunishmentRows(result);
			  const PunishmentLoadOutcome outcome =
				  ApplyPunishmentRows(rows, m_config, Now(), /*forced=*/true, m_punishments[static_cast<size_t>(slot)]);
			  const int64_t now = Now();
			  for (const PunishmentRow& row : rows) {
				  if (m_config.punish_server_id != -1 && row.server_id != m_config.punish_server_id && row.server_id != -1) {
					  continue;
				  }
				  const int type = static_cast<int>(row.punish_type);
				  if (type < 0 || type >= kPunishmentTypes) {
					  continue;
				  }
				  if (row.expires != 0 && row.expires < now) {
					  continue;
				  }
				  // PARITY: the forced reload reported a database id (-1 for the console row)
				  // where the live path passes a slot.
				  const int admin_ref = row.admin_db_id == 1 ? -1 : static_cast<int>(row.admin_db_id);
				  const int time = row.expires > 0 ? static_cast<int>(row.expires - row.created) : 0;
				  if (onPlayerPunish) {
					  onPlayerPunish(slot, type, time, row.reason, admin_ref);
				  }
			  }
			  if (onPunishmentsLoaded) {
				  onPunishmentsLoaded(slot, outcome);
			  }
		  });
}

// --- removing punishments ---------------------------------------------------------------

bool AdminCore::MayUnpunish(int admin_slot, uint64_t punishment_admin_steamid) const {
	if (m_config.unpunish_type != 0) {
		return true;
	}
	if (admin_slot == -1) {
		return true; // console
	}
	if (HasPermission(admin_slot, "@admin/unpunish_all")) {
		return true;
	}
	// Intended rule (see the header): only the admin that issued the punishment may lift it.
	return punishment_admin_steamid != 0 && punishment_admin_steamid == AdminSteamId(admin_slot);
}

bool AdminCore::TryUnpunish(int slot, int type, int admin_slot) {
	if (!SlotInRange(slot) || type < 0 || type >= kPunishmentTypes) {
		return false;
	}
	if (!MayUnpunish(admin_slot, PunishmentAdminSteamId(slot, type))) {
		return false;
	}
	Unpunish(slot, type, admin_slot);
	return true;
}

void AdminCore::Unpunish(int slot, int type, int admin_slot) {
	if (!SlotInRange(slot) || type < 0 || type >= kPunishmentTypes) {
		return;
	}
	const uint64_t steamid = m_admins[static_cast<size_t>(slot)].steamid;
	if (steamid == 0) {
		ReportError("admin_system: refusing to lift a punishment of slot " + std::to_string(slot) + " without a steamid");
		return;
	}
	Query("UPDATE {p}punishments SET unpunish_admin_id = ? WHERE steamid = ? AND punish_type = ? "
		  "AND (expires > ? OR expires = 0) AND server_id = ?",
		  {AdminDbId(admin_slot), SteamIdText(steamid), static_cast<int64_t>(type), Now(),
		   static_cast<int64_t>(m_config.punish_server_id)},
		  [this](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: lifting a punishment failed: " + result.Error());
			  }
		  });

	m_punishments[static_cast<size_t>(slot)][static_cast<size_t>(type)] = Punishment{};
	if (onPlayerUnpunish) {
		onPlayerUnpunish(slot, type, admin_slot);
	}
}

void AdminCore::UnpunishOffline(std::string steamid64, int type, int admin_slot) {
	if (steamid64.empty() || type < 0 || type >= kPunishmentTypes) {
		return;
	}
	std::string sql =
		"UPDATE {p}punishments SET unpunish_admin_id = ? WHERE steamid = ? AND punish_type = ? "
		"AND (expires > ? OR expires = 0)";
	std::vector<plgdb::Value> params{AdminDbId(admin_slot), steamid64, static_cast<int64_t>(type), Now()};
	if (m_config.unpunish_type == 0 && admin_slot != -1 && !HasPermission(admin_slot, "@admin/unpunish_all")) {
		// PARITY: the original restricted the UPDATE to the caller's own punishments.
		sql += " AND admin_id = ?";
		params.emplace_back(AdminDbId(admin_slot));
	}
	sql += " AND server_id = ?";
	params.emplace_back(static_cast<int64_t>(m_config.punish_server_id));

	Query(std::move(sql), std::move(params), [this](const plgdb::Result& result) {
		if (!result.Ok()) {
			ReportError("admin_system: lifting an offline punishment failed: " + result.Error());
		}
	});
	if (onOfflinePlayerUnpunish) {
		onOfflinePlayerUnpunish(steamid64, type, admin_slot);
	}
}

void AdminCore::RemoveExpiredAdmins() {
	Query(BuildAdminSelect() + " WHERE s.expires > 0 AND s.expires < ?", {Now()}, [this](const plgdb::Result& result) {
		if (!result.Ok()) {
			ReportError("admin_system: listing the expired admins failed: " + result.Error());
			return;
		}
		const std::vector<AdminRow> rows = ParseAdminRows(result);
		for (const AdminRow& row : rows) {
			Query("DELETE FROM {p}admins_servers WHERE admin_id = ? AND server_id = ?",
				  {row.admin_id, row.server_id}, [this](const plgdb::Result& inner) {
					  if (!inner.Ok()) {
						  ReportError("admin_system: deleting an expired admin row failed: " + inner.Error());
					  }
				  });
		}
	});
}

// --- admin maintenance --------------------------------------------------------------------

void AdminCore::AddAdmin(std::string name, std::string steamid64, std::string flags, int immunity, int expire_seconds,
						 int64_t group_id, std::string comment, bool db) {
	if (steamid64.empty()) {
		return;
	}
	const uint64_t steamid = ParseU64(steamid64);
	std::vector<int> slots = (steamid != 0 && findOnlineSlots) ? findOnlineSlots(steamid) : std::vector<int>{};
	// PARITY: the original gave up when the target was online and already an admin.
	for (const int slot : slots) {
		if (FindAdmin(slot) != nullptr && m_admins[static_cast<size_t>(slot)].id != 0) {
			return;
		}
	}

	const int64_t expires = expire_seconds == 0 ? kPermanent : Now() + expire_seconds;
	if (db) {
		Query("SELECT id FROM {p}admins WHERE steamid = ?", {steamid64},
			  [this, steamid64, flags, immunity, expires, group_id, name, comment, slots](const plgdb::Result& result) {
				  if (!result.Ok()) {
					  ReportError("admin_system: looking up the admin row of " + steamid64 + " failed: " + result.Error());
					  return;
				  }
				  if (result.RowCount() > 0) {
					  AddAdminServerRow(result.GetInt(0, 0, 0), steamid64, flags, immunity, expires, group_id, name, slots);
					  return;
				  }
				  Query("INSERT INTO {p}admins (name, steamid, comment) VALUES (?, ?, ?)", {name, steamid64, comment},
						[this, steamid64, flags, immunity, expires, group_id, name, slots](const plgdb::Result& insert) {
							if (!insert.Ok()) {
								ReportError("admin_system: creating the admin row of " + steamid64 + " failed: " + insert.Error());
								return;
							}
							const int64_t admin_id = static_cast<int64_t>(insert.InsertId());
							Query("SELECT id FROM {p}admins WHERE steamid = ?", {steamid64},
								  [this, steamid64, flags, immunity, expires, group_id, name, slots, admin_id](const plgdb::Result& lookup) {
									  if (!lookup.Ok() || lookup.RowCount() == 0) {
										  if (!lookup.Ok()) {
											  ReportError("admin_system: reading the new admin row failed: " + lookup.Error());
										  }
										  return;
									  }
									  AddAdminServerRow(lookup.GetInt(0, 0, admin_id), steamid64, flags, immunity, expires, group_id, name, slots);
								  });
						});
			  });
		return;
	}

	// bDB = false: only an online target changes, and only in memory.
	if (slots.empty()) {
		return;
	}
	// PARITY: the original built this record with the console id and did not expand the flags.
	for (const int slot : slots) {
		AdminRecord record;
		record.id = 1;
		record.steamid = steamid;
		record.name = name;
		record.immunity = immunity;
		record.expire_time = expire_seconds == 0 ? kPermanent : expires;
		for (const char c : flags) {
			record.flags.emplace_back(1, c);
		}
		m_admins[static_cast<size_t>(slot)] = std::move(record);
		if (group_id != 0) {
			// PARITY: the original read "SELECT * FROM groups WHERE id = ?" and copied the id,
			// the name, the flags and the immunity of that row into the in-memory record.
			Query("SELECT id, name, flags, immunity FROM {p}groups WHERE id = ?", {group_id},
				  [this, slot](const plgdb::Result& result) {
					  if (!result.Ok() || result.RowCount() == 0) {
						  return;
					  }
					  AdminRecord& admin = m_admins[static_cast<size_t>(slot)];
					  admin.group_id = result.GetInt(0, 0, 0);
					  admin.group_name = result.GetString(0, 1);
					  if (const int immunity = static_cast<int>(result.GetInt(0, 3, 0)); immunity > admin.immunity) {
						  admin.immunity = immunity;
					  }
					  AppendFlagsAndPermissions(admin, result.GetString(0, 2), m_config);
				  });
		}
		if (onAdminLoaded) {
			onAdminLoaded(slot, m_admins[static_cast<size_t>(slot)]);
		}
	}
}

void AdminCore::AddAdminServerRow(int64_t admin_id, const std::string& steamid64, const std::string& flags, int immunity,
								  int64_t expires, int64_t group_id, const std::string& name, const std::vector<int>& slots) {
	const std::string insert = m_db.GetDriver() == plgdb::Driver::MySQL
								   ? "INSERT IGNORE INTO {p}admins_servers (admin_id, flags, immunity, expires, group_id, server_id) VALUES (?, ?, ?, ?, ?, ?)"
								   : "INSERT OR IGNORE INTO {p}admins_servers (admin_id, flags, immunity, expires, group_id, server_id) VALUES (?, ?, ?, ?, ?, ?)";
	Query(insert, {admin_id, flags, static_cast<int64_t>(immunity), expires, group_id, static_cast<int64_t>(m_config.admin_server_id)},
		  [this, slots, steamid64, name](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: storing the admin row of " + steamid64 + " failed: " + result.Error());
				  return;
			  }
			  const uint64_t steamid = ParseU64(steamid64);
			  for (const int slot : slots) {
				  LoadAdmin(slot, steamid, name);
			  }
		  });
}

void AdminCore::RemoveAdmin(std::string steamid64, bool db) {
	if (steamid64.empty()) {
		return;
	}
	const uint64_t steamid = ParseU64(steamid64);
	if (db) {
		std::string remove = "DELETE FROM {p}admins_servers WHERE admin_id = (SELECT id FROM {p}admins WHERE steamid = ?)";
		if (m_config.admin_server_id != -1) {
			remove += " AND server_id = ?";
		}
		Query(std::move(remove), m_config.admin_server_id != -1
									? plgdb::Params{steamid64, static_cast<int64_t>(m_config.admin_server_id)}
									: plgdb::Params{steamid64},
			  [this, steamid64](const plgdb::Result& result) {
				  if (!result.Ok()) {
					  ReportError("admin_system: removing the admin row of " + steamid64 + " failed: " + result.Error());
					  return;
				  }
				  // The shared as_admins row only goes away when no server uses it any more.
				  Query("SELECT id FROM {p}admins_servers WHERE admin_id = (SELECT id FROM {p}admins WHERE steamid = ?)",
						{steamid64}, [this, steamid64](const plgdb::Result& remaining) {
							if (!remaining.Ok() || remaining.RowCount() > 0) {
								return;
							}
							Query("DELETE FROM {p}admins WHERE steamid = ?", {steamid64}, [this](const plgdb::Result& deleted) {
								if (!deleted.Ok()) {
									ReportError("admin_system: deleting the admin row failed: " + deleted.Error());
								}
							});
						});
			  });
	}
	if (steamid != 0 && findOnlineSlots) {
		for (const int slot : findOnlineSlots(steamid)) {
			if (SlotInRange(slot)) {
				m_admins[static_cast<size_t>(slot)].Clear();
			}
		}
	}
}

void AdminCore::AddGroup(std::string name, std::string flags, int immunity, std::function<void(int64_t)> onInserted) {
	if (name.empty()) {
		return;
	}
	Query("INSERT INTO {p}groups (name, flags, immunity) VALUES (?, ?, ?)", {name, flags, static_cast<int64_t>(immunity)},
		  [this, callback = std::move(onInserted)](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: creating a group failed: " + result.Error());
				  return;
			  }
			  if (callback) {
				  callback(static_cast<int64_t>(result.InsertId()));
			  }
		  });
}

void AdminCore::RemoveGroup(std::string identifier) {
	if (identifier.empty()) {
		return;
	}
	const bool by_id = OnlyDigits(identifier);
	Query(by_id ? "DELETE FROM {p}groups WHERE id = ?" : "DELETE FROM {p}groups WHERE name = ?",
		  by_id ? plgdb::Params{static_cast<int64_t>(ParseU64(identifier))} : plgdb::Params{std::move(identifier)},
		  [this](const plgdb::Result& result) {
			  if (!result.Ok()) {
				  ReportError("admin_system: deleting a group failed: " + result.Error());
			  }
		  });
}

} // namespace admin_system
