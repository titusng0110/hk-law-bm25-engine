#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <cctype>
#include <ranges>
#include <algorithm>
#include <iterator>

// External libraries
#include "utf8.h"             // utf8cpp
#include "libstemmer.h"       // Snowball C API

// Helper: Check if a codepoint is a digit
constexpr bool is_digit(char32_t cp) {
    return cp >= U'0' && cp <= U'9';
}

// Helper: Check if a codepoint is alphanumeric (ASCII letters or digits)
constexpr bool is_alnum(char32_t cp) {
    return is_digit(cp) || (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z');
}

// Helper: Check if a codepoint is Chinese
constexpr bool is_cjk(char32_t cp) {
    return (cp >= 0x4E00  && cp <= 0x9FFF)  ||
           (cp >= 0x3400  && cp <= 0x4DBF)  ||
           (cp >= 0x20000 && cp <= 0x2A6DF) ||
           (cp >= 0x2A700 && cp <= 0x2B73F) ||
           (cp >= 0x2B740 && cp <= 0x2B81F) ||
           (cp >= 0x2B820 && cp <= 0x2CEAF) ||
           (cp >= 0x2CEB0 && cp <= 0x2EBEF) ||
           (cp >= 0x30000 && cp <= 0x3134F);
}

// Helper: Fast check for Roman Numerals up to 100 (i to c)
constexpr bool is_roman_numeral_upto_100(std::string_view s) {
    if (s.empty()) return false;
    size_t pos = 0;

    // Tens (0 to 100)
    if (s.starts_with("xc")) pos += 2;
    else if (s.starts_with("c")) pos += 1; // 100
    else if (s.starts_with("lxxx")) pos += 4;
    else if (s.starts_with("lxx")) pos += 3;
    else if (s.starts_with("lx")) pos += 2;
    else if (s.starts_with("l")) pos += 1;
    else if (s.starts_with("xl")) pos += 2;
    else if (s.starts_with("xxx")) pos += 3;
    else if (s.starts_with("xx")) pos += 2;
    else if (s.starts_with("x")) pos += 1;

    // Ones (0 to 9)
    if (pos < s.length()) {
        std::string_view rem = s.substr(pos);
        if (rem == "ix") pos += 2;
        else if (rem == "viii") pos += 4;
        else if (rem == "vii") pos += 3;
        else if (rem == "vi") pos += 2;
        else if (rem == "v") pos += 1;
        else if (rem == "iv") pos += 2;
        else if (rem == "iii") pos += 3;
        else if (rem == "ii") pos += 2;
        else if (rem == "i") pos += 1;
    }

    return pos == s.length();
}

enum class StatKeywordType { None, Abbreviation, FullWord };

constexpr StatKeywordType get_statutory_keyword_type(std::string_view s) {
    // Abbreviations (joined with a dot, e.g., "ss.2(1)")
    if (s == "s" || s == "ss" || s == "sub-s" || s == "sub-ss" ||
        s == "para" || s == "paras" || s == "subpara" || s == "subparas" ||
        s == "pt" || s == "div" || s == "sch" || s == "schs" ||
        s == "r" || s == "o" || s == "cap" || s == "art") {
        return StatKeywordType::Abbreviation;
    }
    // Full words (joined with a space, e.g., "part a")
    if (s == "section" || s == "sections" || s == "subsection" || s == "subsections" ||
        s == "paragraph" || s == "paragraphs" || s == "subparagraph" || s == "subparagraphs" ||
        s == "part" || s == "division" || s == "schedule" || s == "schedules" ||
        s == "rule" || s == "order" || s == "chapter" || s == "article" ||
        s == "clause" || s == "provision" || s == "term" || s == "appendix" || s == "annex"/*contract keywords*/) {
        return StatKeywordType::FullWord;
    }
    return StatKeywordType::None;
}

// Helper: Check if the inner content of brackets is valid for citation merging
constexpr bool is_valid_citation_token(std::string_view s) {
    if (s.empty()) return false;

    // Rule 1: Roman numeral up to 100
    if (is_roman_numeral_upto_100(s)) return true;

    // Rule 2, 3, & 4: Pure digits, OR digits + 1 letter, OR exactly 1 letter
    bool all_digits_until_last = true;
    for (size_t i = 0; i < s.length() - 1; ++i) {
        if (s[i] < '0' || s[i] > '9') {
            all_digits_until_last = false;
            break;
        }
    }

    if (all_digits_until_last) {
        char last = s.back();
        bool is_last_digit = (last >= '0' && last <= '9');
        bool is_last_letter = (last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z');

        if (is_last_digit || is_last_letter) {
            return true;
        }
    }

    return false;
}

// C++20/23 Transparent Hash
struct StringHash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view txt) const {
        return std::hash<std::string_view>{}(txt);
    }
};
using FastSet = std::unordered_set<std::string, StringHash, std::equal_to<>>;

FastSet load_set(std::string_view filename) {
    FastSet data;
    std::ifstream file(filename.data());
    std::string line;
    while (std::getline(file, line)) {
        if (line.ends_with('\r')) line.pop_back();
        if (!line.empty()) data.insert(std::move(line));
    }
    return data;
}

int main() {
    FastSet stopwords = load_set("lexisnexis_stopwords.txt");

    struct sb_stemmer* stemmer = sb_stemmer_new("english", "UTF_8");
    if (!stemmer) return 1;

    std::ifstream infile("input.txt");
    std::ofstream outfile("output.txt");
    if (!infile.is_open() || !outfile.is_open()) return 1;

    std::string line;
    std::string current_english_word;

    std::vector<std::string> tokens;
    std::vector<std::string> final_tokens;
    std::vector<std::string> statutory_tokens;
    tokens.reserve(20000);
    final_tokens.reserve(20000);
    statutory_tokens.reserve(20000);

    while (std::getline(infile, line)) {
        if (line.empty()) {
            outfile << "[]\n";
            continue;
        }
        tokens.clear();
        current_english_word.clear();

        // ==========================================
        // PART 2: THE "FLUSH" LAMBDA
        // ==========================================
        auto flush_english_word = [&]() {
            if (current_english_word.empty()) return;

            bool is_citation = is_valid_citation_token(current_english_word);
            StatKeywordType stat_type = get_statutory_keyword_type(current_english_word);
            bool is_stat_keyword = (stat_type != StatKeywordType::None);

            // Prevent stopword drop for citation tokens & keywords
            if (!is_stat_keyword && !is_citation) {
                if (stopwords.contains(current_english_word)) {
                    current_english_word.clear();
                    return;
                }
            }

            // Skip stemming so keywords/citations don't get mangled
            if (!is_stat_keyword && !is_citation) {
                const sb_symbol* stemmed = sb_stemmer_stem(
                    stemmer,
                    reinterpret_cast<const sb_symbol*>(current_english_word.c_str()),
                    current_english_word.length()
                );
                current_english_word = reinterpret_cast<const char*>(stemmed);
            }

            tokens.push_back(std::move(current_english_word));
            current_english_word.clear();
        };

        // ==========================================
        // PART 1: THE CHARACTER LOOP (Abridged for spacing)
        // ==========================================
        auto it = line.begin();
        auto end = line.end();
        char32_t prev_cp = 0;
        std::string prev_cjk_char = "";
        bool last_was_cjk = false;

        while (it != end) {
            char32_t cp = utf8::next(it, end);
            char32_t next_cp = (it != end) ? utf8::peek_next(it, end) : 0;

            // 1. Flatten Accents
            if (cp == U'ß') {
                utf8::append(U's', std::back_inserter(current_english_word));
                cp = U's'; // Second 's' will be appended in the is_alnum block
            } else if (cp == U'æ' || cp == U'Æ') {
                utf8::append(U'a', std::back_inserter(current_english_word));
                cp = U'e';
            } else if (cp == U'œ' || cp == U'Œ') {
                utf8::append(U'o', std::back_inserter(current_english_word));
                cp = U'e';
            }
            switch (cp) {
                // A / a
                case U'Á': case U'À': case U'Â': case U'Ä': case U'Ã': case U'Å': case U'Ā': case U'Ă': case U'Ą': cp = U'A'; break;
                case U'á': case U'à': case U'â': case U'ä': case U'ã': case U'å': case U'ā': case U'ă': case U'ą': cp = U'a'; break;
                // C / c
                case U'Ç': case U'Ć': case U'Č': cp = U'C'; break;
                case U'ç': case U'ć': case U'č': cp = U'c'; break;
                // E / e
                case U'É': case U'È': case U'Ê': case U'Ë': case U'Ē': case U'Ė': case U'Ę': case U'Ě': cp = U'E'; break;
                case U'é': case U'è': case U'ê': case U'ë': case U'ē': case U'ė': case U'ę': case U'ě': cp = U'e'; break;
                // I / i
                case U'Í': case U'Ì': case U'Î': case U'Ï': case U'Ī': case U'Į': cp = U'I'; break;
                case U'í': case U'ì': case U'î': case U'ï': case U'ī': case U'į': cp = U'i'; break;
                // N / n
                case U'Ñ': case U'Ń': case U'Ň': cp = U'N'; break;
                case U'ñ': case U'ń': case U'ň': cp = U'n'; break;
                // O / o
                case U'Ó': case U'Ò': case U'Ô': case U'Ö': case U'Õ': case U'Ø': case U'Ō': cp = U'O'; break;
                case U'ó': case U'ò': case U'ô': case U'ö': case U'õ': case U'ø': case U'ō': cp = U'o'; break;
                // U / u
                case U'Ú': case U'Ù': case U'Û': case U'Ü': case U'Ū': case U'Ů': case U'Ų': cp = U'U'; break;
                case U'ú': case U'ù': case U'û': case U'ü': case U'ū': case U'ů': case U'ų': cp = U'u'; break;
                // Y / y
                case U'Ý': case U'Ÿ': cp = U'Y'; break;
                case U'ý': case U'ÿ': cp = U'y'; break;
                // S / s (Common in Eastern European names)
                case U'Ś': case U'Š': case U'Ş': cp = U'S'; break;
                case U'ś': case U'š': case U'ş': cp = U's'; break;
                // Z / z
                case U'Ź': case U'Ż': case U'Ž': cp = U'Z'; break;
                case U'ź': case U'ż': case U'ž': cp = U'z'; break;
            }

            // 2. Alphanumerics
            if (is_alnum(cp)) {
                if (cp >= U'A' && cp <= U'Z') cp += 32; // Lowercase
                current_english_word.push_back(static_cast<char>(cp));
                last_was_cjk = false;
            }
            // 3. Decimals
            else if (cp == U'.') {
                if (is_digit(prev_cp) && is_digit(next_cp)) {
                    utf8::append(cp, std::back_inserter(current_english_word));
                } else {
                    flush_english_word();
                }
                last_was_cjk = false;
            }
            // 4. Commas in numbers
            else if (cp == U',') {
                if (!is_digit(prev_cp) || !is_digit(next_cp)) flush_english_word();
                last_was_cjk = false;
            }
            // 5. Hyphens, Slashes and Ampersands
            else if (cp == U'-' || cp == U'/' || cp == U'&') {
                if (is_alnum(prev_cp) && is_alnum(next_cp)) {
                    utf8::append(cp, std::back_inserter(current_english_word));
                } else {
                    flush_english_word();
                }
                last_was_cjk = false;
            }
            // 6. Extract Parentheses as Standalone Tokens for Phase 2
            else if (cp == U'(' || cp == U')' || cp == 0xFF08 || cp == 0xFF09) { // 0xFF08/0xFF09 are '（' and '）'
                flush_english_word();
                char normalized_bracket = (cp == U'(' || cp == 0xFF08) ? '(' : ')';
                tokens.push_back(std::string(1, normalized_bracket));
                last_was_cjk = false;
            }
            // 7. CJK
            else if (is_cjk(cp)) {
                flush_english_word();

                std::string current_cjk_char;
                utf8::append(cp, std::back_inserter(current_cjk_char));

                // Push the unigram
                tokens.push_back(current_cjk_char);

                // Push the overlapping bigram
                if (last_was_cjk) {
                    tokens.push_back(prev_cjk_char + current_cjk_char);
                }

                prev_cjk_char = current_cjk_char;
                last_was_cjk = true;
            }
            // 8. Delimiters
            else {
                flush_english_word();
                last_was_cjk = false;
            }
            prev_cp = cp;
        }
        flush_english_word();

        // ==========================================
        // PART 3: PHASE 2 - BRACKET MERGING
        // ==========================================
        final_tokens.clear();
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == "(" && !final_tokens.empty() && i + 2 < tokens.size() && tokens[i+2] == ")") {
                const std::string& prev = final_tokens.back();
                const std::string& inner = tokens[i+1];

                // The previous token is valid if it's a direct match (e.g., "a", "522a")
                // OR if it's an already chained bracket (e.g., "522a(1)").
                bool prev_is_valid = prev.ends_with(')') || is_valid_citation_token(prev);

                if (prev_is_valid && is_valid_citation_token(inner)) {
                    final_tokens.back() += "(" + inner + ")";
                    i += 2;
                    continue;
                }
            }
            if (tokens[i] != "(" && tokens[i] != ")") {
                final_tokens.push_back(std::move(tokens[i]));
            }
        }
        tokens = std::move(final_tokens);

        // ==========================================
        // PART 4: PHASE 3 - STATUTORY MERGING
        // ==========================================
        statutory_tokens.clear();
        for (size_t i = 0; i < tokens.size(); ++i) {
            StatKeywordType stat_type = get_statutory_keyword_type(tokens[i]);

            if (stat_type != StatKeywordType::None && i + 1 < tokens.size()) {
                const std::string& next_token = tokens[i+1];

                // Extract the base target (everything before the first '(') to validate
                std::string_view base_target = next_token;
                size_t bracket_pos = base_target.find('(');
                if (bracket_pos != std::string_view::npos) {
                    base_target = base_target.substr(0, bracket_pos);
                }

                if (is_valid_citation_token(base_target)) {
                    std::string merged = tokens[i];
                    if (stat_type == StatKeywordType::Abbreviation) {
                        merged += "." + next_token; // "s.522a(1)"
                    } else {
                        merged += " " + next_token; // "section 522a(1)"
                    }
                    statutory_tokens.push_back(std::move(merged));
                    ++i; // Skip the merged target
                    continue;
                }
            }
            statutory_tokens.push_back(std::move(tokens[i]));
        }
        tokens = std::move(statutory_tokens);

        // Print Output
        outfile << "[";
        for (size_t i = 0; i < tokens.size(); ++i) {
            outfile << "\"" << tokens[i] << "\"";
            if (i < tokens.size() - 1) outfile << ", ";
        }
        outfile << "]\n";
    }

    sb_stemmer_delete(stemmer);
    return 0;
}
