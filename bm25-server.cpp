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
#include <queue> // Added for std::priority_queue

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
// Read-only after the loading phase, safe for concurrent access.

std::vector<double> precomputed_K; // Replaces the doc_lengths hash map
std::unordered_map<std::string, TermData> inverted_index;
double avgdl = 1.0;
uint32_t N = 0;
uint32_t max_doc_id = 0; // Track the highest document ID to size our flat arrays

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

    // Temporary vector to hold lengths until avgdl is calculated
    std::vector<uint32_t> temp_doc_lengths;

    while (std::getline(fdocs, line)) {
        if (line.empty()) continue;
        try {
            json j = json::parse(line);
            uint32_t id = j.at("id").get<uint32_t>();
            uint32_t D = j.at("D").get<uint32_t>();

            if (id > max_doc_id) max_doc_id = id;
            if (id >= temp_doc_lengths.size()) {
                temp_doc_lengths.resize(id + 10000, 0); // Batch resize to avoid thrashing
            }

            temp_doc_lengths[id] = D;
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

            // Precompute IDF
            double idf = std::log(((N - df + 0.5) / (df + 0.5)) + 1.0);

            TermData td;
            td.idf = idf;

            const auto& p_array = j.at("p");
            td.postings.reserve(p_array.size());
            for (const auto& p : p_array) {
                uint32_t doc_id = p[0].get<uint32_t>();
                if (doc_id > max_doc_id) max_doc_id = doc_id; // Safety check
                td.postings.push_back({doc_id, p[1].get<uint32_t>()});
            }

            inverted_index[t] = std::move(td);
            term_count++;
        } catch (const json::exception& e) {
            std::cerr << "JSON error parsing index.jsonl: " << e.what() << "\n";
        }
    }
    std::cout << "Loaded " << term_count << " terms into the index.\n";

    // 4. Precompute BM25 Length Penalties (Memory Locality Optimization)
    std::cout << "Precomputing BM25 penalties for O(1) inner loop..." << std::endl;
    precomputed_K.assign(max_doc_id + 1, 0.0);
    for (uint32_t i = 0; i <= max_doc_id; ++i) {
        uint32_t D = (i < temp_doc_lengths.size() && temp_doc_lengths[i] > 0) ? temp_doc_lengths[i] : avgdl;
        precomputed_K[i] = k1 * (1.0 - b + b * (static_cast<double>(D) / avgdl));
    }

    // Free the temporary lengths array
    temp_doc_lengths.clear();
    temp_doc_lengths.shrink_to_fit();

    // 5. Setup and Start HTTP Server
    httplib::Server svr;

    auto search_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::istringstream iss(req.body);
        std::string req_line;
        std::string response_body;

        // --- Thread-Local Resources (Zero Allocations per Request) ---
        thread_local std::unique_ptr<bm25::Tokenizer> tokenizer;
        if (!tokenizer) {
            tokenizer = std::make_unique<bm25::Tokenizer>("lexisnexis_stopwords.txt");
        }

        // Fast flat array for scoring, allocated exactly once per thread
        thread_local std::vector<double> doc_scores;
        thread_local std::vector<uint32_t> touched_docs;

        // Ensure thread-local arrays are sized properly
        if (doc_scores.size() <= max_doc_id) {
            doc_scores.assign(max_doc_id + 1, 0.0);
        }

        // Process incoming JSON lines
        while (std::getline(iss, req_line)) {
            if (req_line.empty()) continue;

            try {
                json j_req = json::parse(req_line);

                if (!j_req.contains("query") || !j_req.at("query").is_string()) {
                    throw std::invalid_argument("Field 'query' must be a string.");
                }
                if (!j_req.contains("k") || !j_req.at("k").is_number_integer() || j_req.at("k").get<int>() < 1) {
                    throw std::invalid_argument("Field 'k' must be a positive integer.");
                }

                std::string query = j_req.at("query").get<std::string>();
                size_t k = j_req.at("k").get<size_t>();

                std::vector<std::pair<std::string, uint32_t>> query_tokens;
                tokenizer->tokenize(query, query_tokens);

                // --- 1. The Ultra-Fast Scoring Loop ---
                for (const auto& q_token : query_tokens) {
                    const std::string& term = q_token.first;
                    uint32_t qtf = q_token.second;

                    auto it = inverted_index.find(term);
                    if (it != inverted_index.end()) {
                        double idf_qtf = it->second.idf * qtf; // Pre-multiply
                        double numerator_base = k1 + 1.0;

                        for (const auto& posting : it->second.postings) {
                            uint32_t doc_id = posting.doc_id;
                            uint32_t tf = posting.tf;

                            // Track docs we've modified so we can fast-reset them later
                            if (doc_scores[doc_id] == 0.0) {
                                touched_docs.push_back(doc_id);
                            }

                            // Math is simplified to one addition and one division, cache-friendly
                            double numerator = tf * numerator_base;
                            double denominator = tf + precomputed_K[doc_id];

                            doc_scores[doc_id] += idf_qtf * (numerator / denominator);
                        }
                    }
                }

                // --- 2. Fast Top-K using Min-Heap ---
                using DocScore = std::pair<uint32_t, double>;

                // Min-Heap comparator: keeps smallest score (or largest ID on tie) at the top
                auto cmp = [](const DocScore& left, const DocScore& right) {
                    if (std::abs(left.second - right.second) < 1e-9) return left.first < right.first;
                    return left.second > right.second;
                };
                std::priority_queue<DocScore, std::vector<DocScore>, decltype(cmp)> top_k_heap(cmp);

                for (uint32_t doc_id : touched_docs) {
                    double score = doc_scores[doc_id];

                    if (top_k_heap.size() < k) {
                        top_k_heap.push({doc_id, score});
                    } else if (score > top_k_heap.top().second ||
                              (std::abs(score - top_k_heap.top().second) < 1e-9 && doc_id < top_k_heap.top().first)) {
                        top_k_heap.pop();
                        top_k_heap.push({doc_id, score});
                    }

                    // FAST CLEANUP: zero out the score array instantly for the next query
                    doc_scores[doc_id] = 0.0;
                }
                touched_docs.clear();

                // --- 3. Extract and Format Output ---
                std::vector<DocScore> results;
                results.reserve(top_k_heap.size());
                while (!top_k_heap.empty()) {
                    results.push_back(top_k_heap.top());
                    top_k_heap.pop();
                }
                // Heap extraction gives us smallest to largest, so we reverse it
                std::reverse(results.begin(), results.end());

                json j_out = json::array();
                for (const auto& result : results) {
                    j_out.push_back({
                        {"id", result.first},
                        {"score", result.second}
                    });
                }

                response_body += j_out.dump() + "\n";

            } catch (const std::exception& e) {
                json j_error = {{"error", e.what()}};
                response_body += j_error.dump() + "\n";
            }
        }

        res.set_content(response_body, "application/x-ndjson");
    };

    svr.Post("/search", search_handler);

    std::cout << "Server starting on 0.0.0.0:" << port << "...\n";
    svr.listen("0.0.0.0", port);

    return 0;
}
