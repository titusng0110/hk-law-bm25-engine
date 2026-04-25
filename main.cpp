#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <cctype>
#include <ranges>     // C++23 Ranges
#include <algorithm>  // C++23 Range Algorithms

// External libraries
#include "utf8.h"             // utf8cpp
#include "libstemmer.h"       // Snowball C API

// Helper: Check if a codepoint is a digit
constexpr bool is_digit(char32_t cp) {
    return cp >= U'0' && cp <= U'9';
}

// Helper: Check if a codepoint is Chinese (Traditional OR Simplified)
constexpr bool is_cjk(char32_t cp) {
    return (cp >= 0x4E00  && cp <= 0x9FFF)  || // CJK Unified Ideographs
           (cp >= 0x3400  && cp <= 0x4DBF)  || // Extension A
           (cp >= 0x20000 && cp <= 0x2A6DF) || // Extension B
           (cp >= 0x2A700 && cp <= 0x2B73F) || // Extension C
           (cp >= 0x2B740 && cp <= 0x2B81F) || // Extension D
           (cp >= 0x2B820 && cp <= 0x2CEAF) || // Extension E
           (cp >= 0x2CEB0 && cp <= 0x2EBEF) || // Extension F
           (cp >= 0x30000 && cp <= 0x3134F);   // Extension G
}

// C++20/23 Transparent Hash to avoid string allocations on lookups
struct StringHash {
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view txt) const {
        return std::hash<std::string_view>{}(txt);
    }
};
using FastSet = std::unordered_set<std::string, StringHash, std::equal_to<>>;

// Helper: Load text files into RAM
FastSet load_set(std::string_view filename) {
    FastSet data;
    std::ifstream file(filename.data());
    std::string line;
    while (std::getline(file, line)) {
        if (line.ends_with('\r')) line.pop_back(); // C++20/23 ends_with
        if (!line.empty()) data.insert(std::move(line));
    }
    return data;
}

int main() {
    std::cout << "Loading English Dictionary and Stop Words into RAM...\n";
    FastSet dictionary = load_set("corncob-lowercase.txt");
    FastSet stopwords  = load_set("lexisnexis_stopwords.txt");

    // Initialize Snowball Stemmer
    struct sb_stemmer* stemmer = sb_stemmer_new("english", "UTF_8");
    if (!stemmer) {
        std::cerr << "Failed to initialize Snowball stemmer.\n";
        return 1;
    }

    std::cout << "Engine ready. Type text and press Enter (Ctrl+C to quit):\n> ";
    std::string line;

    // Main Engine Loop
    while (std::getline(std::cin, line)) {
        std::vector<std::string> tokens;
        std::string current_english_word;

        // ==========================================
        // PART 2: THE "FLUSH" LAMBDA
        // ==========================================
        auto flush_english_word = [&]() {
            if (current_english_word.empty()) return;

            // 1. Stop Word Check FIRST (C++20/23 .contains())
            if (stopwords.contains(current_english_word)) {
                current_english_word.clear();
                return;
            }

            // 2. Check for Digits (C++23 Range Algorithms)
            bool has_digit = std::ranges::any_of(current_english_word, [](char c) {
                return std::isdigit(static_cast<unsigned char>(c));
            });

            // 3. Dictionary Check & Stem
            if (!has_digit) {
                if (dictionary.contains(current_english_word)) {
                    // Standard word -> Stem it
                    const sb_symbol* stemmed = sb_stemmer_stem(
                        stemmer,
                        reinterpret_cast<const sb_symbol*>(current_english_word.c_str()),
                        current_english_word.length()
                    );
                    current_english_word = reinterpret_cast<const char*>(stemmed);
                }
            }

            // 4. Emit
            tokens.push_back(std::move(current_english_word));
            current_english_word.clear();
        };

        // ==========================================
        // PART 1: THE CHARACTER LOOP
        // ==========================================
        auto it = line.begin();
        auto end = line.end();
        char32_t prev_cp = 0;

        while (it != end) {
            char32_t cp = utf8::next(it, end);
            char32_t next_cp = (it != end) ? utf8::peek_next(it, end) : 0;

            // 1. Flatten Accents
            switch (cp) {
                case U'é': case U'è': case U'ê': case U'ë': cp = U'e'; break;
                case U'É': case U'È': case U'Ê': case U'Ë': cp = U'E'; break;
                case U'á': case U'à': case U'â': case U'ä': cp = U'a'; break;
                case U'Á': case U'À': case U'Â': case U'Ä': cp = U'A'; break;
                case U'ç': cp = U'c'; break;
                case U'Ç': cp = U'C'; break;
            }

            // 2. Alphanumerics
            if ((cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z') || is_digit(cp)) {
                if (cp >= U'A' && cp <= U'Z') cp += 32; // Lowercase
                utf8::append(cp, current_english_word);
            }
            // 3. Decimals
            else if (cp == U'.') {
                if (is_digit(prev_cp) && is_digit(next_cp)) {
                    utf8::append(cp, current_english_word);
                } else {
                    flush_english_word();
                }
            }
            // 4. Commas in numbers
            else if (cp == U',') {
                if (!is_digit(prev_cp) || !is_digit(next_cp)) {
                    flush_english_word();
                }
            }
            // 5. CJK
            else if (is_cjk(cp)) {
                flush_english_word();
                std::string cjk_token;
                utf8::append(cp, cjk_token);
                tokens.push_back(std::move(cjk_token));
            }
            // 6. Delimiters
            else {
                flush_english_word();
            }
            prev_cp = cp;
        }
        flush_english_word();

        // Print Output (Back to standard C++ to avoid MinGW <print> bugs)
        std::cout << "Tokens: [";
        for (size_t i = 0; i < tokens.size(); ++i) {
            std::cout << "\"" << tokens[i] << "\"";
            if (i < tokens.size() - 1) std::cout << ", ";
        }
        std::cout << "]\n> ";
    }

    sb_stemmer_delete(stemmer);
    return 0;
}
