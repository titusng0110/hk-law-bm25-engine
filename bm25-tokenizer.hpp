#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <stdexcept>
#include <cctype>
#include <iterator>
#include <algorithm>
#include <utility>

// External libraries
#include "utf8.h"             // utf8cpp
#include "libstemmer.h"       // Snowball C API

namespace bm25 {

namespace detail {
    // Virtual Tokens for Gap Tracking
    constexpr std::string_view V_SPA = "<SPA>";
    constexpr std::string_view V_PER = "<PER>";
    constexpr std::string_view V_DRO = "<DRO>";

    constexpr bool is_virtual(std::string_view s) {
        return s == V_SPA || s == V_PER || s == V_DRO;
    }

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

        if (s.starts_with("xc")) pos += 2;
        else if (s.starts_with("c")) pos += 1;
        else if (s.starts_with("lxxx")) pos += 4;
        else if (s.starts_with("lxx")) pos += 3;
        else if (s.starts_with("lx")) pos += 2;
        else if (s.starts_with("l")) pos += 1;
        else if (s.starts_with("xl")) pos += 2;
        else if (s.starts_with("xxx")) pos += 3;
        else if (s.starts_with("xx")) pos += 2;
        else if (s.starts_with("x")) pos += 1;

        if (pos < s.length()) {
            std::string_view rem = s.substr(pos);
            if (rem.starts_with("ix")) pos += 2;
            else if (rem.starts_with("viii")) pos += 4;
            else if (rem.starts_with("vii")) pos += 3;
            else if (rem.starts_with("vi")) pos += 2;
            else if (rem.starts_with("v")) pos += 1;
            else if (rem.starts_with("iv")) pos += 2;
            else if (rem.starts_with("iii")) pos += 3;
            else if (rem.starts_with("ii")) pos += 2;
            else if (rem.starts_with("i")) pos += 1;
        }

        // Must have matched at least one Roman numeral character
        if (pos == 0) return false;

        // Count trailing letters (to allow HK 'Part VIA', 'Part IVA', but block long garbage strings)
        size_t letter_count = 0;
        for (size_t i = pos; i < s.length(); ++i) {
            if (s[i] < 'a' || s[i] > 'z') return false;
            letter_count++;
        }

        return letter_count <= 1; // Change to '== 0' if you want to ban trailing letters entirely!
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
            s == "clause" || s == "provision" || s == "term" || s == "appendix" || s == "annex") {
            return StatKeywordType::FullWord;
        }
        return StatKeywordType::None;
    }

    // Helper: Check if the inner content of brackets is valid for citation merging
    constexpr bool is_valid_citation_token(std::string_view s) {
        if (s.empty()) return false;

        // Rule 1: Roman numeral (+ max 1 appendix letter)
        if (is_roman_numeral_upto_100(s)) return true;

        // Rule 2: Starts with a digit, followed by MAXIMUM 2 letters. No digits after letters.
        if (s[0] >= '0' && s[0] <= '9') {
            size_t letter_count = 0;
            bool letters_started = false;

            for (char c : s) {
                if (c >= '0' && c <= '9') {
                    // If we already saw a letter, another digit is illegal (e.g., "32a1" fails)
                    if (letters_started) return false;
                } else if (c >= 'a' && c <= 'z') {
                    letters_started = true;
                    letter_count++;
                } else {
                    // Fails on symbols or punctuation
                    return false;
                }
            }
            return letter_count <= 2; // Validates "32", "32w", "32aa". Fails "32aaa".
        }

        // Rule 3: Single standalone letters (e.g., Appendix A, Schedule B)
        if (s.length() == 1 && s[0] >= 'a' && s[0] <= 'z') {
            return true;
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
} // namespace detail

class Tokenizer {
private:
    using FastSet = std::unordered_set<std::string, detail::StringHash, std::equal_to<>>;

    FastSet stopwords;
    struct sb_stemmer* stemmer = nullptr;

    // NOW TRACKING POSITIONS: std::pair<token_string, absolute_position>
    using PosToken = std::pair<std::string, uint32_t>;

    // Pre-allocated vectors to prevent reallocation during bulk indexing
    std::vector<PosToken> raw_tokens;
    std::vector<PosToken> phase2_tokens;
    std::vector<PosToken> phase3_tokens;
    std::string current_english_word;

    static FastSet load_set(std::string_view filename) {
        FastSet data;
        std::ifstream file(filename.data());
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open stopwords file: " + std::string(filename));
        }
        std::string line;
        while (std::getline(file, line)) {
            if (line.ends_with('\r')) line.pop_back();
            if (!line.empty()) data.insert(std::move(line));
        }
        return data;
    }

public:
    explicit Tokenizer(std::string_view stopwords_path) {
        stopwords = load_set(stopwords_path);
        stemmer = sb_stemmer_new("english", "UTF_8");
        if (!stemmer) {
            throw std::runtime_error("Failed to initialize Snowball stemmer");
        }

        // Pre-reserve capacity to speed up initial documents
        raw_tokens.reserve(20000);
        phase2_tokens.reserve(20000);
        phase3_tokens.reserve(20000);
        current_english_word.reserve(100);
    }

    ~Tokenizer() {
        if (stemmer) {
            sb_stemmer_delete(stemmer);
        }
    }

    Tokenizer(const Tokenizer&) = delete;
    Tokenizer& operator=(const Tokenizer&) = delete;

    Tokenizer(Tokenizer&& other) noexcept
        : stopwords(std::move(other.stopwords)),
          stemmer(other.stemmer),
          raw_tokens(std::move(other.raw_tokens)),
          phase2_tokens(std::move(other.phase2_tokens)),
          phase3_tokens(std::move(other.phase3_tokens))
    {
        other.stemmer = nullptr;
    }

    // Convenience wrapper returning the vector
    std::vector<std::pair<std::string, uint32_t>> tokenize(std::string_view text) {
        std::vector<std::pair<std::string, uint32_t>> out_tokens;
        tokenize(text, out_tokens);
        return out_tokens;
    }

    // Main tokenization loop. Modifies out_tokens in place to save allocations.
    void tokenize(std::string_view text, std::vector<std::pair<std::string, uint32_t>>& out_tokens) {
        out_tokens.clear();
        if (text.empty()) return;

        raw_tokens.clear();
        current_english_word.clear();

        uint32_t absolute_pos = 0; // CRITICAL: The running position tracker

        // ==========================================
        // PART 1: THE FLUSHING & VIRTUAL LAMBDAS
        // ==========================================
        auto flush_english_word = [&]() {
            if (current_english_word.empty()) return;

            bool is_citation = detail::is_valid_citation_token(current_english_word);
            detail::StatKeywordType stat_type = detail::get_statutory_keyword_type(current_english_word);
            bool is_stat_keyword = (stat_type != detail::StatKeywordType::None);

            if (!is_stat_keyword && !is_citation) {
                if (stopwords.contains(current_english_word)) {
                    current_english_word.clear();
                    absolute_pos++; // CONSUME POSITION EVEN IF DROPPED
                    return;
                }
            }

            if (!is_stat_keyword && !is_citation) {
                const sb_symbol* stemmed = sb_stemmer_stem(
                    stemmer,
                    reinterpret_cast<const sb_symbol*>(current_english_word.c_str()),
                    current_english_word.length()
                );
                current_english_word = reinterpret_cast<const char*>(stemmed);
            }

            raw_tokens.emplace_back(std::move(current_english_word), absolute_pos);
            absolute_pos++; // Advance for next word
            current_english_word.clear();
        };

        // Collapses consecutive virtual tokens into the "strongest" one
        auto emit_virtual = [&](std::string_view v) {
            if (raw_tokens.empty()) return;
            if (detail::is_virtual(raw_tokens.back().first)) {
                if (v == detail::V_DRO) {
                    raw_tokens.back().first = detail::V_DRO;
                } else if (v == detail::V_PER && raw_tokens.back().first == detail::V_SPA) {
                    raw_tokens.back().first = detail::V_PER;
                }
            } else {
                // Virtual tokens are dropped later, position 0 is just a placeholder
                raw_tokens.emplace_back(std::string(v), 0);
            }
        };

        // ==========================================
        // PART 2: THE CHARACTER LOOP
        // ==========================================
        auto it = text.begin();
        auto end = text.end();
        char32_t prev_cp = 0;
        std::string prev_cjk_char = "";
        bool last_was_cjk = false;

        while (it != end) {
            char32_t cp = utf8::next(it, end);
            char32_t next_cp = (it != end) ? utf8::peek_next(it, end) : 0;

            // 1. Flatten Accents
            if (cp == U'ß') {
                utf8::append(U's', std::back_inserter(current_english_word));
                cp = U's';
            } else if (cp == U'æ' || cp == U'Æ') {
                utf8::append(U'a', std::back_inserter(current_english_word));
                cp = U'e';
            } else if (cp == U'œ' || cp == U'Œ') {
                utf8::append(U'o', std::back_inserter(current_english_word));
                cp = U'e';
            }
            switch (cp) {
                case U'Á': case U'À': case U'Â': case U'Ä': case U'Ã': case U'Å': case U'Ā': case U'Ă': case U'Ą': cp = U'A'; break;
                case U'á': case U'à': case U'â': case U'ä': case U'ã': case U'å': case U'ā': case U'ă': case U'ą': cp = U'a'; break;
                case U'Ç': case U'Ć': case U'Č': cp = U'C'; break;
                case U'ç': case U'ć': case U'č': cp = U'c'; break;
                case U'É': case U'È': case U'Ê': case U'Ë': case U'Ē': case U'Ė': case U'Ę': case U'Ě': cp = U'E'; break;
                case U'é': case U'è': case U'ê': case U'ë': case U'ē': case U'ė': case U'ę': case U'ě': cp = U'e'; break;
                case U'Í': case U'Ì': case U'Î': case U'Ï': case U'Ī': case U'Į': cp = U'I'; break;
                case U'í': case U'ì': case U'î': case U'ï': case U'ī': case U'į': cp = U'i'; break;
                case U'Ñ': case U'Ń': case U'Ň': cp = U'N'; break;
                case U'ñ': case U'ń': case U'ň': cp = U'n'; break;
                case U'Ó': case U'Ò': case U'Ô': case U'Ö': case U'Õ': case U'Ø': case U'Ō': cp = U'O'; break;
                case U'ó': case U'ò': case U'ô': case U'ö': case U'õ': case U'ø': case U'ō': cp = U'o'; break;
                case U'Ú': case U'Ù': case U'Û': case U'Ü': case U'Ū': case U'Ů': case U'Ų': cp = U'U'; break;
                case U'ú': case U'ù': case U'û': case U'ü': case U'ū': case U'ů': case U'ų': cp = U'u'; break;
                case U'Ý': case U'Ÿ': cp = U'Y'; break;
                case U'ý': case U'ÿ': cp = U'y'; break;
                case U'Ś': case U'Š': case U'Ş': cp = U'S'; break;
                case U'ś': case U'š': case U'ş': cp = U's'; break;
                case U'Ź': case U'Ż': case U'Ž': cp = U'Z'; break;
                case U'ź': case U'ż': case U'ž': cp = U'z'; break;
            }

            // 2. Alphanumerics
            if (detail::is_alnum(cp)) {
                if (cp >= U'A' && cp <= U'Z') cp += 32;
                current_english_word.push_back(static_cast<char>(cp));
                last_was_cjk = false;
            }
            // 3. Decimals
            else if (cp == U'.') {
                if (detail::is_digit(prev_cp) && detail::is_digit(next_cp)) {
                    // It's a true decimal like 3.14
                    utf8::append(cp, std::back_inserter(current_english_word));
                } else {
                    // Check if the word before the period is an abbreviation
                    bool is_abbrev = false;
                    if (!current_english_word.empty()) {
                        auto stat_type = detail::get_statutory_keyword_type(current_english_word);
                        if (stat_type == detail::StatKeywordType::Abbreviation) {
                            is_abbrev = true;
                        } else if (current_english_word.length() <= 2) {
                            // Catch general short abbreviations like "v.", "Mr.", "U.S."
                            is_abbrev = true;
                        }
                    }

                    flush_english_word();
                    emit_virtual(detail::V_PER);

                    // ONLY apply the sentence penalty if it wasn't an abbreviation!
                    if (!is_abbrev) {
                        absolute_pos += 8; // CROSS-SENTENCE BLEED PENALTY
                    }
                }
                last_was_cjk = false;
            }
            // 4. Commas in numbers
            else if (cp == U',') {
                if (detail::is_digit(prev_cp) && detail::is_digit(next_cp)) {
                    utf8::append(cp, std::back_inserter(current_english_word));
                } else {
                    flush_english_word();
                    emit_virtual(detail::V_DRO);
                }
                last_was_cjk = false;
            }
            // 5. Hyphens, Slashes and Ampersands
            else if (cp == U'-' || cp == U'/' || cp == U'&') {
                if (detail::is_alnum(prev_cp) && detail::is_alnum(next_cp)) {
                    utf8::append(cp, std::back_inserter(current_english_word));
                } else {
                    flush_english_word();
                    emit_virtual(detail::V_DRO);
                }
                last_was_cjk = false;
            }
            // 6. Extract Parentheses as Standalone Tokens
            else if (cp == U'(' || cp == U')' || cp == 0xFF08 || cp == 0xFF09) {
                flush_english_word();
                char normalized_bracket = (cp == U'(' || cp == 0xFF08) ? '(' : ')';
                raw_tokens.emplace_back(std::string(1, normalized_bracket), absolute_pos);
                absolute_pos++; // Brackets consume a position
                last_was_cjk = false;
            }
            // 7. CJK (With Position Stacking for Bigrams)
            else if (detail::is_cjk(cp)) {
                flush_english_word();
                std::string current_cjk_char;
                utf8::append(cp, std::back_inserter(current_cjk_char));

                // Unigram gets current absolute position
                raw_tokens.emplace_back(current_cjk_char, absolute_pos);

                // Bigram gets STACKED on the PREVIOUS character's position
                if (last_was_cjk) {
                    raw_tokens.emplace_back(prev_cjk_char + current_cjk_char, absolute_pos - 1);
                }

                prev_cjk_char = current_cjk_char;
                absolute_pos++; // Advance position for next char
                last_was_cjk = true;
            }
            // 8. Delimiters
            else {
                flush_english_word();
                if (cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' || cp == 0x3000) {
                    emit_virtual(detail::V_SPA);
                } else if (cp == 0x3002 || cp == 0xFF0E) { // Asian Full Stops
                    emit_virtual(detail::V_PER);
                    absolute_pos += 8; // CROSS-SENTENCE BLEED PENALTY
                } else {
                    emit_virtual(detail::V_DRO);
                }
                last_was_cjk = false;
            }
            prev_cp = cp;
        }
        flush_english_word();

        // ==========================================
        // PART 3: PHASE 2 - BRACKET MERGING
        // ==========================================
        phase2_tokens.clear();
        for (size_t i = 0; i < raw_tokens.size(); ++i) {
            if (raw_tokens[i].first == "(") {
                size_t inner_idx = -1;
                size_t close_idx = -1;
                int tokens_checked = 0;

                for (size_t j = i + 1; j < raw_tokens.size(); ++j) {
                    if (detail::is_virtual(raw_tokens[j].first)) continue;

                    if (tokens_checked >= 2) break;

                    if (inner_idx == static_cast<size_t>(-1)) {
                        inner_idx = j;
                        tokens_checked++;
                    } else if (close_idx == static_cast<size_t>(-1)) {
                        if (raw_tokens[j].first == ")") {
                            close_idx = j;
                        }
                        tokens_checked++;
                        break;
                    }
                }

                if (inner_idx != static_cast<size_t>(-1) && close_idx != static_cast<size_t>(-1) && detail::is_valid_citation_token(raw_tokens[inner_idx].first)) {
                    size_t prev_idx = phase2_tokens.size();
                    while (prev_idx > 0 && detail::is_virtual(phase2_tokens[prev_idx - 1].first)) {
                        prev_idx--;
                    }

                    if (prev_idx > 0) {
                        auto& prev = phase2_tokens[prev_idx - 1];
                        bool prev_is_valid = prev.first.ends_with(')') || detail::is_valid_citation_token(prev.first);

                        if (prev_is_valid) {
                            // Append string, but retain the ORIGINAL anchor position!
                            prev.first += "(" + raw_tokens[inner_idx].first + ")";
                            i = close_idx; // Jump indices
                            continue;
                        }
                    }
                }
            }

            if (raw_tokens[i].first != "(" && raw_tokens[i].first != ")") {
                phase2_tokens.push_back(std::move(raw_tokens[i]));
            }
        }

        // ==========================================
        // PART 4: PHASE 3 - STATUTORY MERGING
        // ==========================================
        phase3_tokens.clear();
        for (size_t i = 0; i < phase2_tokens.size(); ++i) {
            // NEVER pass virtual tokens into the final array
            if (detail::is_virtual(phase2_tokens[i].first)) {
                continue;
            }

            detail::StatKeywordType stat_type = detail::get_statutory_keyword_type(phase2_tokens[i].first);

            if (stat_type != detail::StatKeywordType::None) {
                size_t next_real_idx = static_cast<size_t>(-1);
                std::string_view gap_type = "";

                for (size_t j = i + 1; j < phase2_tokens.size(); ++j) {
                    if (detail::is_virtual(phase2_tokens[j].first)) {
                        if (gap_type != detail::V_DRO) {
                            if (phase2_tokens[j].first == detail::V_DRO) gap_type = detail::V_DRO;
                            else if (phase2_tokens[j].first == detail::V_PER) gap_type = detail::V_PER;
                            else if (phase2_tokens[j].first == detail::V_SPA && gap_type == "") gap_type = detail::V_SPA;
                        }
                    } else {
                        next_real_idx = j;
                        break;
                    }
                }

                if (next_real_idx != static_cast<size_t>(-1)) {
                    const std::string& next_token = phase2_tokens[next_real_idx].first;

                    std::string_view base_target = next_token;
                    size_t bracket_pos = base_target.find('(');
                    if (bracket_pos != std::string_view::npos) {
                        base_target = base_target.substr(0, bracket_pos);
                    }

                    if (detail::is_valid_citation_token(base_target)) {
                        bool can_merge = false;

                        if (stat_type == detail::StatKeywordType::FullWord) {
                            if (gap_type == "" || gap_type == detail::V_SPA) can_merge = true;
                        } else if (stat_type == detail::StatKeywordType::Abbreviation) {
                            if (gap_type == "" || gap_type == detail::V_SPA || gap_type == detail::V_PER) can_merge = true;
                        }

                        if (can_merge) {
                            std::string merged = phase2_tokens[i].first;
                            if (stat_type == detail::StatKeywordType::Abbreviation) {
                                merged += "." + next_token;
                            } else {
                                merged += " " + next_token;
                            }
                            // Emplace Bigram STACKED on the Unigram's anchor position
                            phase3_tokens.emplace_back(std::move(merged), phase2_tokens[i].second);

                            // Deliberately allow the unigram to fall through and be pushed below as well
                        }
                    }
                }
            }
            // Push the unigram (or already merged token from Phase 2)
            phase3_tokens.push_back(std::move(phase2_tokens[i]));
        }

        // ==========================================
        // PART 5: TRANSFER TO OUTPUT
        // ==========================================
        if (phase3_tokens.empty()) return;

        out_tokens.reserve(phase3_tokens.size());

        for (auto& token : phase3_tokens) {
            out_tokens.push_back(std::move(token));
        }
    }
};

} // namespace bm25
