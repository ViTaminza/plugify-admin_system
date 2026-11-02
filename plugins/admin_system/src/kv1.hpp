// admin_system — the self-contained Valve KeyValues (KV1) reader used for core.ini, sorting.ini,
// databases.cfg, core.cfg and admin_system.phrases.txt.
//
// Self-contained on purpose (docs/PLUGIN_STANDARD.md §7: no pisexcompat, no helper library): the
// Metamod admin core loaded its files through hl2sdk's KeyValues and looked the keys up with
// GetString/GetInt; this header keeps the file format and those getter semantics, so the
// production files are read unchanged.
//
// Accepted grammar (Valve's): quoted ("key" "value") and unquoted (key value) tokens, a key may
// carry a value and a block, nested blocks, // and /* */ comments, \" \\ \n \r \t escapes
// inside quotes, and #base / #include / #define lines are skipped instead of followed. Like
// Valve's loader, a file that is a single "Config" / "Databases" / "Phrases" block is returned as
// *that block's contents*, which is why GetString("database_prefix") reads a key inside
// "Config" { ... } and why the reason/time tables are read from the "reasons"/"times" blocks. A
// malformed file - an unterminated string, a missing '}' or a stray top-level '}' - is rejected as
// a whole (std::nullopt), so half a config never overrides the defaults. A UTF-8 BOM is ignored.
//
// GetInt() is the original's getter: the value is trimmed, an empty/missing value returns the
// caller's default, and a parse failure falls back to atoi ("5 // x" -> 5, "1.5" -> 1, "abc" -> 0).
// GetBool() is KeyValues' truthiness: a missing key returns the default, "true"/"yes"/"on" are
// true, "false"/"no"/"off" are false and any other non-empty value is `atoi(value) != 0`.
//
// isBlock/value mirror KeyValues' GetFirstTrueSubKey() / GetFirstValue() split that the Pisex
// loops (FOR_EACH_SUBKEY / FOR_EACH_VALUE) relied on, so the "flags" block is read as blocks and
// "default_permissions" as values.
//
// The parser is the one plugins/as_checkcheats and plugins/as_blockdb carry as well (each port
// keeps its own copy so the plugin repository stays self-contained).

#pragma once

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace admin_system {

// --- 1. the KV1 reader ------------------------------------------------------------------------

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	bool isBlock = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive, like Valve's FindKey), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or carries no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
	// The child's value as an int with the original's atoi fallback; `def` when the child is
	// missing or its value is empty.
	int GetInt(std::string_view key, int def = 0) const;
	// The child's boolean ("true"/"yes"/"on" -> true, "false"/"no"/"off" -> false, otherwise the
	// integer test); `def` when the child is missing or its value is empty.
	bool GetBool(std::string_view key, bool def = false) const;
};

namespace detail {

inline char LowerCase(char c) {
	return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

inline bool EqualNoCase(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (LowerCase(a[i]) != LowerCase(b[i])) {
			return false;
		}
	}
	return true;
}

inline std::string_view Trim(std::string_view text) {
	std::size_t begin = 0;
	std::size_t end = text.size();
	while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}
	return text.substr(begin, end - begin);
}

// The original's atoi fallback for a non-numeric value ("1.5" -> 1, "abc" -> 0).
inline int Atoi(std::string_view text) {
	const std::string copy(text);
	return std::atoi(copy.c_str());
}

//   file  := node* EOF
//   node  := token [ token ] [ '{' node* '}' ]
//   token := '"' escaped '"' | raw
class Parser {
public:
	explicit Parser(std::string_view text) : text_(text) {}

	bool Parse(KvNode& root) {
		ParseNodes(root, false);
		return !failed_;
	}

private:
	void SkipSpace() {
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c))) {
				++pos_;
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
				pos_ += 2;
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '*') {
				pos_ += 2;
				while (pos_ + 1 < text_.size() && !(text_[pos_] == '*' && text_[pos_ + 1] == '/')) {
					++pos_;
				}
				pos_ = (pos_ + 1 < text_.size()) ? pos_ + 2 : text_.size();
				continue;
			}
			break;
		}
	}

	// One token; false at the end of the input or at a '}'.
	bool ReadToken(std::string& out) {
		SkipSpace();
		if (pos_ >= text_.size() || text_[pos_] == '}') {
			return false;
		}
		out.clear();
		if (text_[pos_] == '"') {
			++pos_;
			while (pos_ < text_.size()) {
				const char c = text_[pos_++];
				if (c == '\\' && pos_ < text_.size()) {
					const char esc = text_[pos_++];
					switch (esc) {
					case 'n':
						out.push_back('\n');
						break;
					case 'r':
						out.push_back('\r');
						break;
					case 't':
						out.push_back('\t');
						break;
					case '"':
						out.push_back('"');
						break;
					case '\\':
						out.push_back('\\');
						break;
					default:
						out.push_back('\\');
						out.push_back(esc);
						break;
					}
					continue;
				}
				if (c == '"') {
					return true;
				}
				out.push_back(c);
			}
			failed_ = true; // unterminated string
			return true;
		}
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (std::isspace(static_cast<unsigned char>(c)) || c == '{' || c == '}' || c == '"') {
				break;
			}
			if (c == '/' && pos_ + 1 < text_.size() && (text_[pos_ + 1] == '/' || text_[pos_ + 1] == '*')) {
				break;
			}
			out.push_back(c);
			++pos_;
		}
		return true;
	}

	bool PeekIs(char c) {
		SkipSpace();
		return pos_ < text_.size() && text_[pos_] == c;
	}

	// Children are appended as soon as they are complete, so the recursion below works on a local
	// node and never holds a reference into a vector that the recursion may reallocate.
	void ParseNodes(KvNode& parent, bool insideBlock) {
		while (true) {
			SkipSpace();
			if (pos_ >= text_.size()) {
				if (insideBlock) {
					failed_ = true; // missing '}'
				}
				return;
			}
			if (text_[pos_] == '}') {
				if (!insideBlock) {
					failed_ = true; // stray '}'
				}
				++pos_;
				return;
			}
			std::string key;
			if (!ReadToken(key)) {
				return;
			}
			if (!key.empty() && key[0] == '#') {
				// #base / #include / #define: no inclusion, skip the line.
				while (pos_ < text_.size() && text_[pos_] != '\n') {
					++pos_;
				}
				continue;
			}
			KvNode node;
			node.name = std::move(key);
			if (!PeekIs('{')) {
				std::string value;
				if (ReadToken(value)) {
					node.value = std::move(value);
					node.hasValue = true;
				}
			}
			if (PeekIs('{')) {
				++pos_;
				node.isBlock = true;
				ParseNodes(node, true);
			}
			parent.children.push_back(std::move(node));
		}
	}

	std::string_view text_;
	std::size_t pos_ = 0;
	bool failed_ = false;
};

} // namespace detail

inline const KvNode* KvNode::Find(std::string_view key) const {
	if (key.empty()) {
		return nullptr;
	}
	for (const KvNode& child : children) {
		if (detail::EqualNoCase(child.name, key)) {
			return &child;
		}
	}
	return nullptr;
}

inline std::string_view KvNode::GetString(std::string_view key, std::string_view def) const {
	if (key.empty()) {
		return hasValue ? std::string_view(value) : def;
	}
	const KvNode* found = Find(key);
	if (found == nullptr || !found->hasValue) {
		return def;
	}
	return std::string_view(found->value);
}

inline int KvNode::GetInt(std::string_view key, int def) const {
	const std::string_view text = detail::Trim(GetString(key, {}));
	if (text.empty()) {
		return def;
	}
	int value = 0;
	const auto* begin = text.data();
	const auto* end = text.data() + text.size();
	const std::from_chars_result result = std::from_chars(begin, end, value);
	if (result.ec != std::errc{}) {
		return detail::Atoi(text); // Valve falls back to atoi semantics
	}
	return value;
}

inline bool KvNode::GetBool(std::string_view key, bool def) const {
	const std::string_view text = detail::Trim(GetString(key, {}));
	if (text.empty()) {
		return def;
	}
	if (detail::EqualNoCase(text, "true") || detail::EqualNoCase(text, "yes") ||
		detail::EqualNoCase(text, "on")) {
		return true;
	}
	if (detail::EqualNoCase(text, "false") || detail::EqualNoCase(text, "no") ||
		detail::EqualNoCase(text, "off")) {
		return false;
	}
	return detail::Atoi(text) != 0;
}

// The file root: an unnamed node whose children are the top-level entries - except that, like
// Valve's loader (and pisexcompat's shim), a file that is a single "Config" / "Core" / "Phrases"
// block is returned as the *contents* of that block, so `GetString("ServerLang")` reads a key
// inside "Core" { ... } and the reason/social tables are read from "end"/"socials".
inline std::optional<KvNode> ParseKv1(std::string_view text) {
	// A UTF-8 BOM would otherwise become part of the first key and hide every entry from lookups.
	constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
	if (text.starts_with(kUtf8Bom)) {
		text.remove_prefix(kUtf8Bom.size());
	}
	KvNode root;
	detail::Parser parser(text);
	if (!parser.Parse(root)) {
		return std::nullopt;
	}
	// The single top-level block becomes the root (its name is only used for logging in Valve).
	if (root.children.size() == 1 && root.children.front().isBlock) {
		KvNode top = std::move(root.children.front());
		root.name = std::move(top.name);
		root.value = std::move(top.value);
		root.hasValue = top.hasValue;
		root.children = std::move(top.children);
	}
	return root;
}

inline std::optional<KvNode> LoadKv1File(const std::string& path) {
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		return std::nullopt;
	}
	std::ostringstream content;
	content << file.rdbuf();
	return ParseKv1(content.str());
}

} // namespace admin_system
