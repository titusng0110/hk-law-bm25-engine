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
#include <cstdio>
#include <cstdint>

#include "nlohmann/json.hpp"
#include "httplib.h"
#include "bm25-tokenizer.hpp"

using json = nlohmann::json;

// --- Global Read-Only State ---
const double k1 = 1.2;
const double b = 0.2;
const uint32_t INF_DOC = 0xFFFFFFFF;

const uint32_t POSTING_RECORD_SIZE = 4;
const uint32_t POSTING_DOC_ID_OFFSET = 0;
const uint32_t POSTING_TF_OFFSET = 1;
const uint32_t POSTING_POS_OFFSET_OFFSET = 2;
const uint32_t POSTING_POS_LENGTH_OFFSET = 3;

const uint32_t DOC_LENGTH_ALLOC_CHUNK = 10000;
const int DEFAULT_PORT = 8080;
const double EPSILON = 1e-9;
const double DEFAULT_AVGDL = 1.0;

const double BM25_IDF_SMOOTHING = 0.5;
const double BM25_IDF_LOG_OFFSET = 1.0;
const double BM25_TF_OFFSET = 1.0;
const double BM25_DL_OFFSET = 1.0;

const uint32_t NEXT_DOC_OFFSET = 1;

const uint32_t MAXSCORE_PRUNE_CANDIDATE_K_MULTIPLIER = 20;
const uint32_t MAXSCORE_PRUNE_CANDIDATE_K_LIMIT = 20000;

const uint32_t SDM_WINDOW_SIZE = 8;
const double SDM_EXACT_BONUS = 1.0;
const double SDM_WINDOW_BONUS = 0.5;
const uint32_t SDM_ADJACENCY = 1;

struct TermDictEntry {
    uint32_t df;
    double max_score;
    uint32_t posting_offset;
    uint32_t posting_length;
};

std::unordered_map<std::string, TermDictEntry> dictionary;
std::vector<uint32_t> global_postings;
std::vector<uint32_t> global_positions;
std::vector<double> precomputed_K;

uint32_t N = 0;
double avgdl = DEFAULT_AVGDL;
uint32_t max_doc_id = 0;

// --- Flat DAAT Query Structures ---

struct QueryTerm {
    double idf;            // 8 bytes
    double max_score;      // 8 bytes
    uint32_t original_pos; // 4 bytes
    uint32_t cursor;       // 4 bytes
    uint32_t end_cursor;   // 4 bytes
    uint32_t current_doc_id; // 4 bytes
    // Total size in memory: 32 bytes (perfectly aligned)

    inline void advance(const uint32_t* postings) {
        cursor += POSTING_RECORD_SIZE; // Skip to next posting [doc_id, tf, pos_offset, pos_length]
        if (cursor < end_cursor) {
            current_doc_id = postings[cursor + POSTING_DOC_ID_OFFSET];
        } else {
            current_doc_id = INF_DOC;
        }
    }

    // Galloping Search (Exponential + Binary Search)
    inline void gallop_to(uint32_t target_doc, const uint32_t* postings) {
        if (current_doc_id >= target_doc) return;

        // use uint64_t to avoid overflow
        uint64_t step = 1;
        uint64_t low = cursor;
        uint64_t high = cursor + step * POSTING_RECORD_SIZE;

        // Exponential phase
        while (high < end_cursor && postings[high] < target_doc) {
            low = high;
            step <<= 1;
            high = low + step * POSTING_RECORD_SIZE;
        }

        if (high > end_cursor) high = end_cursor;

        // Binary search phase within the bounds
        uint32_t count = (high - low) / POSTING_RECORD_SIZE;
        while (count > 0) {
            uint32_t half = count / 2;
            uint32_t mid = low + half * POSTING_RECORD_SIZE;
            if (postings[mid] < target_doc) {
                low = mid + POSTING_RECORD_SIZE;
                count -= half + 1;
            } else {
                count = half;
            }
        }

        cursor = low;
        current_doc_id = (cursor < end_cursor) ? postings[cursor + POSTING_DOC_ID_OFFSET] : INF_DOC;
    }
};

struct MatchedTerm {
    uint32_t original_pos;
    double bm25;
    uint32_t pos_offset;
    uint32_t pos_length;
};

struct DocScore {
    uint32_t doc_id;
    double score;
};

// Min-Heap comparator: Smallest score at top. Ties broken by largest Doc ID.
auto heap_cmp = [](const DocScore& left, const DocScore& right) {
    if (std::abs(left.score - right.score) < EPSILON) return left.doc_id < right.doc_id;
    return left.score > right.score;
};

void print_usage(const char* prog_name) {
    std::cerr << "Usage: " << prog_name << " -i1 docs.jsonl -i2 index.bin -p " << DEFAULT_PORT << std::endl;
}

int main(int argc, char* argv[]) {
    std::string docs_path;
    std::string index_path;
    int port = DEFAULT_PORT;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-i1" && i + 1 < argc) docs_path = argv[++i];
        else if (arg == "-i2" && i + 1 < argc) index_path = argv[++i];
        else if (arg == "-p" && i + 1 < argc) port = std::stoi(argv[++i]);
        else { print_usage(argv[0]); return 1; }
    }

    if (docs_path.empty() || index_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    // ==========================================
    // 1. Load Docs & Precompute BM25 Penalities
    // ==========================================
    std::cout << "Loading docs from " << docs_path << "..." << std::endl;
    std::ifstream fdocs(docs_path);
    if (!fdocs.is_open()) return 1;

    std::string line;
    std::vector<uint32_t> temp_doc_lengths;

    while (std::getline(fdocs, line)) {
        if (line.empty()) continue;
        json j = json::parse(line);
        uint32_t id = j.at("id").get<uint32_t>();
        uint32_t D = j.at("D").get<uint32_t>();

        if (id > max_doc_id) max_doc_id = id;
        if (id >= temp_doc_lengths.size()) temp_doc_lengths.resize(id + DOC_LENGTH_ALLOC_CHUNK, 0);
        temp_doc_lengths[id] = D;
    }

    // ==========================================
    // 2. Load Raw Binary Index
    // ==========================================
    std::cout << "Loading binary index from " << index_path << "..." << std::endl;
    FILE* in_bin = std::fopen(index_path.c_str(), "rb");
    if (!in_bin) {
        std::cerr << "Fatal: Could not open " << index_path << std::endl;
        return 1;
    }

    // Macro to verify that fread succeeds, satisfying the -Wunused-result warning
#define SAFE_READ(ptr, size, count, stream) \
    do { \
        if (std::fread(ptr, size, count, stream) != (count)) { \
            std::cerr << "Fatal: Failed to read from index file." << std::endl; \
            std::fclose(stream); \
            return 1; \
        } \
    } while(false)

    SAFE_READ(&N, sizeof(uint32_t), 1, in_bin);
    SAFE_READ(&avgdl, sizeof(double), 1, in_bin);

    uint32_t num_terms;
    SAFE_READ(&num_terms, sizeof(uint32_t), 1, in_bin);

    dictionary.reserve(num_terms);
    for (uint32_t i = 0; i < num_terms; ++i) {
        uint32_t t_len;
        SAFE_READ(&t_len, sizeof(uint32_t), 1, in_bin);
        std::string term(t_len, '\0');
        SAFE_READ(term.data(), 1, t_len, in_bin);

        TermDictEntry entry;
        SAFE_READ(&entry.df, sizeof(uint32_t), 1, in_bin);
        SAFE_READ(&entry.max_score, sizeof(double), 1, in_bin);
        SAFE_READ(&entry.posting_offset, sizeof(uint32_t), 1, in_bin);
        SAFE_READ(&entry.posting_length, sizeof(uint32_t), 1, in_bin);
        dictionary[term] = entry;
    }

    size_t gpost_size;
    SAFE_READ(&gpost_size, sizeof(size_t), 1, in_bin);
    global_postings.resize(gpost_size);
    SAFE_READ(global_postings.data(), sizeof(uint32_t), gpost_size, in_bin);

    size_t gpos_size;
    SAFE_READ(&gpos_size, sizeof(size_t), 1, in_bin);
    global_positions.resize(gpos_size);
    SAFE_READ(global_positions.data(), sizeof(uint32_t), gpos_size, in_bin);

#undef SAFE_READ

    std::fclose(in_bin);

    // Populate precomputed K
    precomputed_K.assign(max_doc_id + 1, 0.0);
    for (uint32_t i = 0; i <= max_doc_id; ++i) {
        uint32_t D = (i < temp_doc_lengths.size() && temp_doc_lengths[i] > 0) ? temp_doc_lengths[i] : avgdl;
        precomputed_K[i] = k1 * (BM25_DL_OFFSET - b + b * (static_cast<double>(D) / avgdl));
    }
    temp_doc_lengths.clear(); temp_doc_lengths.shrink_to_fit();

    std::cout << "Index loaded. Terms: " << num_terms << ", N: " << N << ", avgdl: " << avgdl << std::endl;

    // ==========================================
    // 3. HTTP Server & Query Handler
    // ==========================================
    httplib::Server svr;

    auto search_handler = [](const httplib::Request& req, httplib::Response& res) {
        std::istringstream iss(req.body);
        std::string req_line, response_body;

        // --- Zero Allocation Hot-Path Resources ---
        thread_local std::unique_ptr<bm25::Tokenizer> tokenizer;
        if (!tokenizer) tokenizer = std::make_unique<bm25::Tokenizer>("lexisnexis_stopwords.txt");

        thread_local std::vector<QueryTerm> q_terms;
        thread_local std::vector<double> prefix_max_score;
        thread_local std::vector<MatchedTerm> matched;
        thread_local std::vector<DocScore> top_k_heap;
        thread_local std::vector<std::pair<std::string, uint32_t>> tokens;

        while (std::getline(iss, req_line)) {
            if (req_line.empty()) continue;
            try {
                json j_req = json::parse(req_line);
                std::string query = j_req.at("query").get<std::string>();
                size_t k = j_req.at("k").get<size_t>();
                size_t candidate_k = std::max(k, std::min(k * MAXSCORE_PRUNE_CANDIDATE_K_MULTIPLIER, static_cast<size_t>(MAXSCORE_PRUNE_CANDIDATE_K_LIMIT)));

                // Reset Thread-Local State
                q_terms.clear();
                prefix_max_score.clear();
                top_k_heap.clear();
                tokens.clear();

                tokenizer->tokenize(query, tokens);

                // Lookup dictionary & construct query state
                for (size_t i = 0; i < tokens.size(); ++i) {
                    const std::string& term = tokens[i].first;
                    auto it = dictionary.find(term);
                    if (it != dictionary.end()) {
                        const auto& entry = it->second;
                        double idf = std::log(BM25_IDF_LOG_OFFSET + (N - entry.df + BM25_IDF_SMOOTHING) / (entry.df + BM25_IDF_SMOOTHING));

                        QueryTerm qt;
                        qt.original_pos = tokens[i].second; // Maintain positional integrity for SDM
                        qt.idf = idf;
                        qt.max_score = entry.max_score;
                        qt.cursor = entry.posting_offset;
                        qt.end_cursor = entry.posting_offset + entry.posting_length;
                        qt.current_doc_id = (qt.cursor < qt.end_cursor) ? global_postings[qt.cursor + POSTING_DOC_ID_OFFSET] : INF_DOC;

                        if (qt.current_doc_id != INF_DOC) {
                            q_terms.push_back(qt);
                        }
                    }
                }

                if (q_terms.empty()) {
                    res.set_content("[]\n", "application/x-ndjson");
                    return;
                }

                // ISSUE 4 FIX: WAND Strict MaxScore implementation requires sorting by max_score descending
                std::sort(q_terms.begin(), q_terms.end(), [](const QueryTerm& lhs, const QueryTerm& rhs) {
                    return lhs.max_score > rhs.max_score;
                });

                // Compute prefix sum of max_scores for pruning
                prefix_max_score.resize(q_terms.size());
                double running_max = 0.0;
                for (int i = q_terms.size() - 1; i >= 0; --i) {
                    running_max += q_terms[i].max_score;
                    prefix_max_score[i] = running_max;
                }

                const uint32_t* p_postings = global_postings.data();
                const uint32_t* p_positions = global_positions.data();
                double heap_min = 0.0;

                // --- 4. The Core DAAT Evaluation Loop ---
                while (true) {
                    // Find the minimum document ID among cursors
                    uint32_t current_doc = INF_DOC;
                    for (const auto& qt : q_terms) {
                        if (qt.current_doc_id < current_doc) current_doc = qt.current_doc_id;
                    }
                    if (current_doc == INF_DOC) break;

                    matched.clear();
                    double doc_score = 0.0;
                    bool pruned = false;

                    for (size_t i = 0; i < q_terms.size(); ++i) {
                        // MaxScore Pruning Check
                        if (doc_score + prefix_max_score[i] < (heap_min - EPSILON) && top_k_heap.size() == candidate_k) {
                            pruned = true;
                            break;
                        }

                        if (q_terms[i].current_doc_id == current_doc) {
                            uint32_t c = q_terms[i].cursor;
                            uint32_t tf = p_postings[c + POSTING_TF_OFFSET];
                            uint32_t pos_offset = p_postings[c + POSTING_POS_OFFSET_OFFSET];
                            uint32_t pos_length = p_postings[c + POSTING_POS_LENGTH_OFFSET];

                            // Base BM25 calculation
                            double bm25 = q_terms[i].idf * (tf * (k1 + BM25_TF_OFFSET)) / (tf + precomputed_K[current_doc]);
                            doc_score += bm25;

                            matched.push_back({q_terms[i].original_pos, bm25, pos_offset, pos_length});

                            // Advance cursor
                            q_terms[i].advance(p_postings);
                        }
                    }

                    if (pruned) {
                        // Gallop remaining cursors that are stuck on the pruned document
                        for (size_t i = 0; i < q_terms.size(); ++i) {
                            if (q_terms[i].current_doc_id == current_doc) {
                                q_terms[i].gallop_to(current_doc + NEXT_DOC_OFFSET, p_postings);
                            }
                        }
                        continue;
                    }

                    // --- 5. Sequential Dependence Model (Proximity Bonuses) ---
                    if (matched.size() > 1) {
                        // Sort matches back to their original query chronological order
                        std::sort(matched.begin(), matched.end(), [](const MatchedTerm& lhs, const MatchedTerm& rhs) {
                            return lhs.original_pos < rhs.original_pos;
                        });

                        double sdm_bonus = 0.0;
                        for (size_t m = 0; m + 1 < matched.size(); ++m) {
                            // Check if adjacent in original query
                            if (matched[m].original_pos + SDM_ADJACENCY == matched[m+1].original_pos) {
                                const auto& tm1 = matched[m];
                                const auto& tm2 = matched[m+1];

                                uint32_t p1 = 0, p2 = 0;
                                bool exact = false;
                                bool window = false;

                                // Two-pointer search through position arrays
                                while (p1 < tm1.pos_length && p2 < tm2.pos_length) {
                                    uint32_t pos1 = p_positions[tm1.pos_offset + p1];
                                    uint32_t pos2 = p_positions[tm2.pos_offset + p2];

                                    if (pos2 == pos1 + SDM_ADJACENCY) {
                                        exact = true; break; // Maximum possible bonus found
                                    }

                                    if (pos1 < pos2) {
                                        if (pos2 - pos1 <= SDM_WINDOW_SIZE) window = true;
                                        p1++;
                                    } else {
                                        if (pos1 - pos2 <= SDM_WINDOW_SIZE) window = true;
                                        p2++;
                                    }
                                }

                                if (exact) {
                                    sdm_bonus += (tm1.bm25 + tm2.bm25) * SDM_EXACT_BONUS; // Adds 1.0x (Multiplier = 2.0x)
                                } else if (window) {
                                    sdm_bonus += (tm1.bm25 + tm2.bm25) * SDM_WINDOW_BONUS; // Adds 0.5x (Multiplier = 1.5x)
                                }
                            }
                        }
                        doc_score += sdm_bonus;
                    }

                    // --- 6. Top-K Min-Heap Insertion ---
                    if (top_k_heap.size() < candidate_k) {
                        top_k_heap.push_back({current_doc, doc_score});
                        std::push_heap(top_k_heap.begin(), top_k_heap.end(), heap_cmp);
                        if (top_k_heap.size() == candidate_k) heap_min = top_k_heap.front().score;
                    } else if (doc_score > heap_min || (std::abs(doc_score - heap_min) < EPSILON && current_doc < top_k_heap.front().doc_id)) {
                        std::pop_heap(top_k_heap.begin(), top_k_heap.end(), heap_cmp);
                        top_k_heap.back() = {current_doc, doc_score};
                        std::push_heap(top_k_heap.begin(), top_k_heap.end(), heap_cmp);
                        heap_min = top_k_heap.front().score;
                    }
                }

                // --- 7. Format JSON Response ---
                // We want descending order: highest score first. Ties broken by smallest doc_id.
                auto result_cmp = [](const DocScore& lhs, const DocScore& rhs) {
                    if (std::abs(lhs.score - rhs.score) < EPSILON) return lhs.doc_id < rhs.doc_id;
                    return lhs.score > rhs.score;
                };

                if (top_k_heap.size() > k) {
                    // Only sort the top 'k' elements into the front of the vector
                    std::partial_sort(top_k_heap.begin(), top_k_heap.begin() + k, top_k_heap.end(), result_cmp);
                    // Discard the rest
                    top_k_heap.resize(k);
                } else {
                    // If we somehow have fewer than or exactly 'k' results, just sort them all
                    std::sort(top_k_heap.begin(), top_k_heap.end(), result_cmp);
                }

                json j_out = json::array();
                for (const auto& result : top_k_heap) {
                    j_out.push_back({
                        {"id", result.doc_id},
                        {"score", result.score}
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

    std::cout << "Server listening on 0.0.0.0:" << port << "..." << std::endl;
    svr.listen("0.0.0.0", port);

    return 0;
}
