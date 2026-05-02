#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Ensure you have this library in your include path
#include "nlohmann/json.hpp"
#include "bm25-tokenizer.hpp"

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json; // Preserve insertion order

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " -i input.jsonl -o1 docs.jsonl -o2 index.bin\n";
}

// Flat struct for the intermediate indexing pass
struct TempPosting {
    uint32_t term_id;
    uint32_t doc_id;
    uint32_t tf;
    uint32_t pos_offset;
    uint32_t pos_length;
};

// Flat struct for local document token parsing
struct DocToken {
    uint32_t term_id;
    uint32_t pos;
};

// Flat struct for final dictionary export
struct TermDictEntry {
    std::string term;
    uint32_t df;
    double max_score;
    uint32_t posting_offset;
    uint32_t posting_length;
};

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

    // 3. Flat Data-Oriented Containers
    std::vector<TempPosting> temp_postings;
    std::vector<uint32_t> global_positions;

    std::unordered_map<std::string, uint32_t> term_to_id;
    std::vector<std::string> id_to_term;
    std::unordered_map<uint32_t, uint32_t> doc_lengths;

    bm25::Tokenizer tokenizer("lexisnexis_stopwords.txt");
    std::vector<std::pair<std::string, uint32_t>> doc_tokens;
    std::vector<DocToken> current_doc_tokens;

    std::string line;
    size_t line_number = 0;
    uint32_t N = 0;
    uint64_t total_length = 0;

    std::cout << "Phase 1: Parsing and building flat positional arrays...\n";

    // ==========================================
    // PHASE 1: DOD Parsing (Append-Only)
    // ==========================================
    while (std::getline(infile, line)) {
        line_number++;
        if (line.empty()) continue;

        try {
            json j_in = json::parse(line);
            uint32_t doc_id = j_in.at("id").get<uint32_t>();
            std::string text = j_in.at("text").get<std::string>();

            doc_tokens.clear();
            current_doc_tokens.clear();
            tokenizer.tokenize(text, doc_tokens);

            uint32_t D = doc_tokens.size();
            doc_lengths[doc_id] = D;
            total_length += D;

            // Map string tokens to integers for fast flat sorting
            for (const auto& token_pair : doc_tokens) {
                const std::string& term = token_pair.first;
                uint32_t pos = token_pair.second;

                auto it = term_to_id.find(term);
                uint32_t tid;
                if (it == term_to_id.end()) {
                    tid = id_to_term.size();
                    term_to_id[term] = tid;
                    id_to_term.push_back(term);
                } else {
                    tid = it->second;
                }
                current_doc_tokens.push_back({tid, pos});
            }

            // Sort document tokens by Term ID, then chronologically by Position
            std::sort(current_doc_tokens.begin(), current_doc_tokens.end(),
                      [](const DocToken& a, const DocToken& b) {
                          if (a.term_id != b.term_id) return a.term_id < b.term_id;
                          return a.pos < b.pos;
                      });

            // Iterate contiguous blocks of term_ids to create temp_postings
            if (!current_doc_tokens.empty()) {
                uint32_t current_term = current_doc_tokens[0].term_id;
                uint32_t tf = 0;
                uint32_t pos_offset = global_positions.size();

                for (const auto& dt : current_doc_tokens) {
                    if (dt.term_id != current_term) {
                        temp_postings.push_back({current_term, doc_id, tf, pos_offset, tf});
                        current_term = dt.term_id;
                        tf = 0;
                        pos_offset = global_positions.size();
                    }
                    global_positions.push_back(dt.pos); // Append to giant positions array
                    tf++;
                }
                // Push trailing term
                temp_postings.push_back({current_term, doc_id, tf, pos_offset, tf});
            }

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

    // ==========================================
    // PHASE 2: Sort Postings & BM25 MaxScore Precomp
    // ==========================================
    std::cout << "Phase 2: Sorting flat indices and calculating BM25 MaxScores...\n";

    // Sort postings by term_id, then doc_id (Strict DAAT Architecture)
    std::sort(temp_postings.begin(), temp_postings.end(),
              [](const TempPosting& a, const TempPosting& b) {
                  if (a.term_id != b.term_id) return a.term_id < b.term_id;
                  return a.doc_id < b.doc_id;
              });

    double avgdl = N > 0 ? static_cast<double>(total_length) / N : 0.0;
    const double k1 = 1.2;
    const double b = 0.75;

    std::vector<uint32_t> global_postings;
    global_postings.reserve(temp_postings.size() * 4); // Format: [doc_id, tf, pos_offset, pos_length]
    std::vector<TermDictEntry> dictionary;

    auto it = temp_postings.begin();
    while (it != temp_postings.end()) {
        uint32_t current_term_id = it->term_id;
        auto group_start = it;
        uint32_t df = 0;

        // Find the length of the contiguous block for this term
        while (it != temp_postings.end() && it->term_id == current_term_id) {
            df++;
            it++;
        }

        // BM25 IDF using standard +0.5 smoothing (strictly positive formulation)
        double idf = std::log(1.0 + (N - df + 0.5) / (df + 0.5));
        double max_score = 0.0;
        uint32_t posting_offset = global_postings.size();

        for (auto p = group_start; p != it; ++p) {
            uint32_t tf = p->tf;
            uint32_t D = doc_lengths[p->doc_id];

            // BM25 Score calculation
            double score = idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * (static_cast<double>(D) / avgdl)));

            // ISSUE 3 FIX: Calculate a theoretical Max SDM Bonus and bake it into max_score.
            // A term can be surrounded by 2 terms in a query (left and right adjacency).
            // Each adjacency adds 1.0x to the base BM25 score.
            // Thus, theoretical max contribution per term is 3.0x its base BM25 score.
            double sdm_theoretical_max = score * 3.0;

            if (sdm_theoretical_max > max_score) {
                max_score = sdm_theoretical_max;
            }

            // Flatten into the giant postings array
            global_postings.push_back(p->doc_id);
            global_postings.push_back(tf);
            global_postings.push_back(p->pos_offset);
            global_postings.push_back(p->pos_length);
        }

        uint32_t posting_length = global_postings.size() - posting_offset;

        dictionary.push_back({
            id_to_term[current_term_id],
            df,
            max_score,
            posting_offset,
            posting_length
        });
    }

    // ==========================================
    // PHASE 3: Raw Binary I/O Export
    // ==========================================
    std::cout << "Phase 3: Serializing index to raw binary file...\n";

    FILE* out_bin = std::fopen(index_output_path.c_str(), "wb");
    if (!out_bin) {
        std::cerr << "Fatal Error: Could not open binary output file " << index_output_path << "\n";
        return 1;
    }

    // 1. Write Header Info (Critical for reconstructing BM25 at query time)
    std::fwrite(&N, sizeof(uint32_t), 1, out_bin);
    std::fwrite(&avgdl, sizeof(double), 1, out_bin);

    uint32_t num_terms = dictionary.size();
    std::fwrite(&num_terms, sizeof(uint32_t), 1, out_bin);

    // 2. Write Dictionary
    for (const auto& entry : dictionary) {
        uint32_t t_len = entry.term.size();
        std::fwrite(&t_len, sizeof(uint32_t), 1, out_bin);
        std::fwrite(entry.term.data(), 1, t_len, out_bin);
        std::fwrite(&entry.df, sizeof(uint32_t), 1, out_bin);
        std::fwrite(&entry.max_score, sizeof(double), 1, out_bin);
        std::fwrite(&entry.posting_offset, sizeof(uint32_t), 1, out_bin);
        std::fwrite(&entry.posting_length, sizeof(uint32_t), 1, out_bin);
    }

    // 3. Write Giant Flattened Arrays
    size_t gpost_size = global_postings.size();
    std::fwrite(&gpost_size, sizeof(size_t), 1, out_bin);
    std::fwrite(global_postings.data(), sizeof(uint32_t), gpost_size, out_bin);

    size_t gpos_size = global_positions.size();
    std::fwrite(&gpos_size, sizeof(size_t), 1, out_bin);
    std::fwrite(global_positions.data(), sizeof(uint32_t), gpos_size, out_bin);

    std::fclose(out_bin);

    std::cout << "Indexing complete. Processed " << line_number << " lines. (" << N << " successful documents).\n";
    std::cout << "Unique terms indexed: " << num_terms << "\n";
    std::cout << "Global Postings length: " << gpost_size << " integers.\n";
    std::cout << "Global Positions length: " << gpos_size << " integers.\n";

    return 0;
}
