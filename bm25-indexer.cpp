#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>

// Ensure you have this library in your include path
#include "nlohmann/json.hpp"
#include "bm25-tokenizer.hpp"

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json; // Added to preserve insertion order

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " -i input.jsonl -o output.jsonl\n";
}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;

    // 1. Parse Command Line Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc) {
            input_path = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            output_path = argv[++i];
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_path.empty() || output_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // 2. Open File Streams
    std::ifstream infile(input_path);
    if (!infile.is_open()) {
        std::cerr << "Error: Could not open input file " << input_path << "\n";
        return 1;
    }

    std::ofstream outfile(output_path);
    if (!outfile.is_open()) {
        std::cerr << "Error: Could not open output file " << output_path << "\n";
        return 1;
    }

    // 3. Process the JSONL line by line
    try {
        // Initialize the tokenizer once
        bm25::Tokenizer tokenizer("lexisnexis_stopwords.txt");

        // Keep ONE token vector allocated to reuse capacity
        std::vector<std::pair<std::string, uint32_t>> doc_tokens;

        // Corpus-level variables for the final output line
        uint32_t N = 0;                                 // Total number of documents
        uint64_t TCT = 0;                               // Total Corpus Tokens
        std::unordered_map<std::string, uint64_t> df;   // Document Frequencies

        std::string line;
        size_t line_number = 0;

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

                // Process tokens to create the TF dictionary, calculate D, and update global df
                json tf_obj = json::object();
                uint32_t D = 0; // Document length

                for (const auto& tf_pair : doc_tokens) {
                    const std::string& term = tf_pair.first;
                    int term_freq = tf_pair.second;

                    tf_obj[term] = term_freq;

                    // Add term frequencies to the document length
                    D += term_freq;

                    // Increment the global Document Frequency for this term
                    df[term]++;
                }

                // Build output JSON object using ordered_json to preserve key order
                ordered_json j_out;

                // Insert exactly in the requested order: id, D, tf, text
                j_out["id"] = doc_id;
                j_out["D"] = D;
                j_out["tf"] = tf_obj; // Note: change this key to "tf" if you prefer
                j_out["text"] = text;

                // Write out the serialized JSON object as a single line
                outfile << j_out.dump() << '\n';

                // Update corpus-level statistics
                TCT += D;
                N++;

            } catch (const json::exception& e) {
                std::cerr << "JSON Parsing Error on line " << line_number << ": " << e.what() << "\n";
            }
        }

        // 4. Write the final line with Corpus Statistics
        ordered_json j_stats;
        j_stats["N"] = N;
        j_stats["TCT"] = TCT;
        j_stats["df"] = df; // nlohmann/json automatically converts unordered_map to a JSON dictionary

        outfile << j_stats.dump() << '\n';

        std::cout << "Indexing complete. Processed " << line_number << " lines. (" << N << " successful documents).\n";

    } catch (const std::exception& e) {
        std::cerr << "Fatal Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
