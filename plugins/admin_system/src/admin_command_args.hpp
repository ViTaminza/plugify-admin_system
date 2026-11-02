// admin_system — the Pisex command-line parser, kept for the ported command handlers.
//
// The Metamod admin core registered its commands with IUtilsApi::RegCommand and parsed the raw
// argument string with CCommand::Tokenize (ArgC()/Arg(i)/ArgS()). s2sdk hands a callback the
// engine's already-tokenized arguments instead, so the handlers could be rewritten - but the
// argument-count checks, the usage strings and the "everything after <time> is the reason" rule of
// the original are *token* based, and keeping the original parser is what keeps the parity
// obvious. The plugin therefore rebuilds the Pisex content string from s2sdk's tokens
// (BuildCommandContent) and feeds it to this parser.
//
// Tokenize() is hl2sdk's CCommand tokenizer: whitespace separated, double quotes group a token and
// are removed, an unterminated quote runs to the end of the string. ArgS() is the unparsed string
// the handler was given, exactly like CCommand::ArgS().
//
// Engine-free: only plg::string (for the s2sdk token vector) and the standard library, so the
// command argument handling is covered by plugins/admin_system/tests/admin_config_tests.cpp.

#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <plg/string.hpp>
#include <plg/vector.hpp>

namespace admin_system {

class CommandArgs {
public:
	CommandArgs() = default;
	explicit CommandArgs(const char* text) { Tokenize(text); }

	// Keeps the raw text (ArgS()) and the parsed tokens.
	void Tokenize(const char* text) {
		text_.assign(text != nullptr ? text : "");
		args_.clear();
		std::size_t i = 0;
		const std::size_t size = text_.size();
		while (i < size) {
			while (i < size && std::isspace(static_cast<unsigned char>(text_[i]))) {
				++i;
			}
			if (i >= size) {
				break;
			}
			std::string token;
			if (text_[i] == '"') {
				++i;
				while (i < size && text_[i] != '"') {
					token.push_back(text_[i++]);
				}
				if (i < size) {
					++i; // closing quote
				}
			} else {
				while (i < size && !std::isspace(static_cast<unsigned char>(text_[i]))) {
					token.push_back(text_[i++]);
				}
			}
			args_.push_back(std::move(token));
		}
	}
	void Tokenize(std::string_view text) { Tokenize(std::string(text).c_str()); }

	int ArgC() const { return static_cast<int>(args_.size()); }
	// nullptr when the index is out of range (CCommand would read past the end).
	const char* Arg(int index) const {
		if (index < 0 || static_cast<std::size_t>(index) >= args_.size()) {
			return nullptr;
		}
		return args_[static_cast<std::size_t>(index)].c_str();
	}
	const char* operator[](int index) const { return Arg(index); }
	// The whole (unparsed) string, as it was passed to Tokenize().
	const char* ArgS() const { return text_.c_str(); }
	void Reset() {
		text_.clear();
		args_.clear();
	}

private:
	std::string text_;
	std::vector<std::string> args_;
};

// s2sdk hands the callback the engine's already-tokenized arguments (quotes stripped), and the
// content is rebuilt by joining them. A token that was quoted by the player has to be quoted again,
// otherwise "mm_ban \"John Doe\" 60" reaches the handler as target "John" (and a quoted reason
// loses its first word). A token that itself contains a double quote is left alone: an escape the
// engine's tokenizer does not understand would only corrupt it differently.
inline std::string QuoteCommandToken(const plg::string& value) {
	const std::string_view token(value.data(), value.size());
	bool needsQuotes = token.empty();
	for (const char c : token) {
		if (std::isspace(static_cast<unsigned char>(c))) {
			needsQuotes = true;
			break;
		}
	}
	if (!needsQuotes || token.find('"') != std::string_view::npos) {
		return std::string(token);
	}
	std::string out;
	out.reserve(token.size() + 2);
	out.push_back('"');
	out.append(token);
	out.push_back('"');
	return out;
}

// "registered spelling" + the arguments from index 1 on, space-joined and re-quoted where needed -
// the string the Pisex handler received through CCommand::ArgS() ("!ban 3 60 cheating").
inline std::string BuildCommandContent(std::string_view spelling, const plg::vector<plg::string>& args) {
	std::string out(spelling);
	for (std::size_t i = 1; i < args.size(); ++i) {
		out.push_back(' ');
		out += QuoteCommandToken(args[i]);
	}
	return out;
}

} // namespace admin_system
