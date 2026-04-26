#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <memory>
#include <stdexcept>

#include "nlohmann/json.hpp"
#include "httplib.h"
#include "bm25-tokenizer.hpp"

using json = nlohmann::json;

// --- Data Structures ---

struct Posting {
    uint32_t doc_id;
    uint32_t tf;
};

struct TermData {
    double idf;
    std::vector<Posting> postings;
};

// --- Global State ---
// Read-only after the loading phase, so safe for concurrent access.

std::unordered_map<uint32_t, uint32_t> doc_lengths;
std::unordered_map<std::string, TermData> inverted_index;
double avgdl = 1.0;
uint32_t N = 0;

// BM25 parameters
const double k1 = 1.2;
const double b = 0.75;

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " -i1 docs.jsonl -i2 index.jsonl -p 8080\n";
}

int main(int argc, char* argv[]) {
    std::string docs_path;
    std::string index_path;
    int port = 8080;

    // 1. Parse Command Line Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i1" && i + 1 < argc) {
            docs_path = argv[++i];
        } else if (arg == "-i2" && i + 1 < argc) {
            index_path = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else {
            std::cerr << "Unknown or incomplete argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (docs_path.empty() || index_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // 2. Load Document Store (for D and avgdl)
    std::cout << "Loading documents from " << docs_path << "..." << std::endl;
    std::ifstream fdocs(docs_path);
    if (!fdocs.is_open()) {
        std::cerr << "Error: Could not open " << docs_path << "\n";
        return 1;
    }

    std::string line;
    uint64_t total_length = 0;
    while (std::getline(fdocs, line)) {
        if (line.empty()) continue;
        try {
            json j = json::parse(line);
            uint32_t id = j.at("id").get<uint32_t>();
            uint32_t D = j.at("D").get<uint32_t>();
            doc_lengths[id] = D;
            total_length += D;
            N++;
        } catch (const json::exception& e) {
            std::cerr << "JSON error parsing docs.jsonl: " << e.what() << "\n";
        }
    }

    if (N > 0) {
        avgdl = static_cast<double>(total_length) / N;
    }
    std::cout << "Loaded " << N << " documents. Average length (avgdl): " << avgdl << "\n";

    // 3. Load Inverted Index
    std::cout << "Loading inverted index from " << index_path << "..." << std::endl;
    std::ifstream findex(index_path);
    if (!findex.is_open()) {
        std::cerr << "Error: Could not open " << index_path << "\n";
        return 1;
    }

    size_t term_count = 0;
    while (std::getline(findex, line)) {
        if (line.empty()) continue;
        try {
            json j = json::parse(line);
            std::string t = j.at("t").get<std::string>();
            uint32_t df = j.at("df").get<uint32_t>();

            // Precompute IDF using standard Lucene/BM25 formula
            double idf = std::log(((N - df + 0.5) / (df + 0.5)) + 1.0);

            TermData td;
            td.idf = idf;

            const auto& p_array = j.at("p");
            td.postings.reserve(p_array.size());
            for (const auto& p : p_array) {
                td.postings.push_back({p[0].get<uint32_t>(), p[1].get<uint32_t>()});
            }

            inverted_index[t] = std::move(td);
            term_count++;
        } catch (const json::exception& e) {
            std::cerr << "JSON error parsing index.jsonl: " << e.what() << "\n";
        }
    }
    std::cout << "Loaded " << term_count << " terms into the index.\n";

    // 4. Setup and Start HTTP Server
    httplib::Server svr;

    auto search_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::istringstream iss(req.body);
        std::string req_line;
        std::string response_body;

        // Use a thread-local tokenizer so that concurrent requests don't interfere
        thread_local std::unique_ptr<bm25::Tokenizer> tokenizer;
        if (!tokenizer) {
            tokenizer = std::make_unique<bm25::Tokenizer>("lexisnexis_stopwords.txt");
        }

        // Process N incoming JSON lines
        while (std::getline(iss, req_line)) {
            if (req_line.empty()) continue;

            try {
                // Parse the individual line
                json j_req = json::parse(req_line);

                // Validate payload to prevent unhandled out-of-range exceptions
                if (!j_req.contains("query") || !j_req.at("query").is_string()) {
                    throw std::invalid_argument("Field 'query' must be a string.");
                }
                if (!j_req.contains("k") || !j_req.at("k").is_number_integer() || j_req.at("k").get<int>() < 1) {
                    throw std::invalid_argument("Field 'k' must be a positive integer.");
                }

                std::string query = j_req.at("query").get<std::string>();
                size_t k = j_req.at("k").get<size_t>();

                // Tokenize query
                std::vector<std::pair<std::string, uint32_t>> query_tokens;
                tokenizer->tokenize(query, query_tokens);

                // Accumulate BM25 scores
                std::unordered_map<uint32_t, double> doc_scores;
                for (const auto& q_token : query_tokens) {
                    const std::string& term = q_token.first;
                    uint32_t qtf = q_token.second; // Frequency of the term in the query

                    auto it = inverted_index.find(term);
                    if (it != inverted_index.end()) {
                        double idf = it->second.idf;
                        for (const auto& posting : it->second.postings) {
                            uint32_t doc_id = posting.doc_id;
                            uint32_t tf = posting.tf;
                            uint32_t D = doc_lengths[doc_id];

                            // Standard BM25 scoring
                            double numerator = tf * (k1 + 1.0);
                            double denominator = tf + k1 * (1.0 - b + b * (static_cast<double>(D) / avgdl));
                            double score_contrib = idf * (numerator / denominator) * qtf;

                            doc_scores[doc_id] += score_contrib;
                        }
                    }
                }

                // Move to a flat list for sorting
                using DocScore = std::pair<uint32_t, double>;
                std::vector<DocScore> top_docs;
                top_docs.reserve(doc_scores.size());
                for (const auto& kv : doc_scores) {
                    top_docs.push_back({kv.first, kv.second});
                }

                // Partial sort to get top-k documents efficiently
                size_t sort_k = std::min(k, top_docs.size());
                std::partial_sort(top_docs.begin(), top_docs.begin() + sort_k, top_docs.end(),
                    [](const DocScore& lhs, const DocScore& rhs) {
                        // Tie breaker based on document ID to ensure deterministic output
                        if (std::abs(lhs.second - rhs.second) < 1e-9) return lhs.first < rhs.first;
                        return lhs.second > rhs.second;
                    });

                // Format as a JSON array and dump to the response line
                json j_out = json::array();
                for (size_t i = 0; i < sort_k; ++i) {
                    j_out.push_back({
                        {"id", top_docs[i].first},
                        {"score", top_docs[i].second}
                    });
                }

                response_body += j_out.dump() + "\n";

            } catch (const std::exception& e) {
                // If a single line fails (validation, missing fields, json parse error)
                // Output a JSON error object for THIS line only.
                json j_error = {{"error", e.what()}};
                response_body += j_error.dump() + "\n";
            }
        }

        // Return the accumulated ndjson string
        res.set_content(response_body, "application/x-ndjson");
    };

    // Bound to /search to match the Python equivalent, but binding to "/" works too.
    svr.Post("/search", search_handler);

    std::cout << "Server starting on 0.0.0.0:" << port << "...\n";
    svr.listen("0.0.0.0", port);

    return 0;
}
