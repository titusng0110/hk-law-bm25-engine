#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>
#include <array>
#include <algorithm> // Added for std::sort

// Ensure you have this library in your include path
#include "nlohmann/json.hpp"
#include "bm25-tokenizer.hpp"

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json; // Preserve insertion order

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " -i input.jsonl -o1 docs.jsonl -o2 index.jsonl\n";
}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string docs_output_path;
    std::string index_output_path;

    // 1. Parse Command Line Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "-o1" && i + 1 < argc) {
            docs_output_path = argv[++i];
        } else if (arg == "-o2" && i + 1 < argc) {
            index_output_path = argv[++i];
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_path.empty() || docs_output_path.empty() || index_output_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // 2. Open File Streams
    std::ifstream infile(input_path);
    if (!infile.is_open()) {
        std::cerr << "Error: Could not open input file " << input_path << "\n";
        return 1;
    }

    std::ofstream outfile_docs(docs_output_path);
    if (!outfile_docs.is_open()) {
        std::cerr << "Error: Could not open docs output file " << docs_output_path << "\n";
        return 1;
    }

    std::ofstream outfile_index(index_output_path);
    if (!outfile_index.is_open()) {
        std::cerr << "Error: Could not open index output file " << index_output_path << "\n";
        return 1;
    }

    // 3. Process the JSONL chunk
    try {
        // Initialize the tokenizer once
        bm25::Tokenizer tokenizer("lexisnexis_stopwords.txt");

        // Keep ONE token vector allocated to reuse capacity
        std::vector<std::pair<std::string, uint32_t>> doc_tokens;

        // In-memory inverted index for this chunk.
        // Maps Term -> List of [doc_id, tf].
        // We use std::array<uint32_t, 2> so nlohmann/json outputs exactly [[id, tf], ...]
        std::unordered_map<std::string, std::vector<std::array<uint32_t, 2>>> inverted_index;

        std::string line;
        size_t line_number = 0;
        uint32_t N = 0;

        while (std::getline(infile, line)) {
            line_number++;

            // Skip empty lines if any exist
            if (line.empty()) continue;

            try {
                // Parse the incoming JSON line
                json j_in = json::parse(line);

                // Extract fields
                uint32_t doc_id = j_in.at("id").get<uint32_t>();
                std::string text = j_in.at("text").get<std::string>();

                // Clear vector before tokenizing
                doc_tokens.clear();

                // Tokenize
                tokenizer.tokenize(text, doc_tokens);

                uint32_t D = 0; // Document length

                // Process tokens to calculate D and update the inverted index
                for (const auto& tf_pair : doc_tokens) {
                    const std::string& term = tf_pair.first;
                    uint32_t term_freq = tf_pair.second;

                    // Add term frequencies to the document length
                    D += term_freq;

                    // Push [doc_id, tf] directly to the inverted index postings list
                    inverted_index[term].push_back({doc_id, term_freq});
                }

                // Build and write out the docs.jsonl JSON object
                ordered_json j_out_docs;
                j_out_docs["id"] = doc_id;
                j_out_docs["D"] = D;
                j_out_docs["text"] = text;

                outfile_docs << j_out_docs.dump() << '\n';

                N++;

            } catch (const json::exception& e) {
                std::cerr << "JSON Parsing Error on line " << line_number << ": " << e.what() << "\n";
            }
        }

        // 4. Sort and Dump the inverted index to index.jsonl

        // Extract pointers to the keys (terms) to avoid making string copies
        std::vector<const std::string*> sorted_terms;
        sorted_terms.reserve(inverted_index.size());
        for (const auto& entry : inverted_index) {
            sorted_terms.push_back(&entry.first);
        }

        // Sort lexicographically by dereferencing the string pointers
        std::sort(sorted_terms.begin(), sorted_terms.end(), [](const std::string* a, const std::string* b) {
            return *a < *b;
        });

        // Iterate through the sorted terms and write to the file
        for (const std::string* term_ptr : sorted_terms) {
            const std::string& term = *term_ptr;
            const auto& postings = inverted_index.at(term);

            ordered_json j_out_index;
            j_out_index["t"] = term;
            j_out_index["df"] = postings.size();
            j_out_index["p"] = postings; // Translates to [[id, tf], [id, tf], ...] automatically

            outfile_index << j_out_index.dump() << '\n';
        }

        std::cout << "Indexing complete. Processed " << line_number << " lines. (" << N << " successful documents).\n";
        std::cout << "Unique terms in inverted index: " << inverted_index.size() << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
