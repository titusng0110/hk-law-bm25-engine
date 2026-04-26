# C++ BM25 Search Engine

A high-performance, lightweight BM25 search engine written in C++23. It consists of an offline **indexer** that builds an inverted index from JSON Lines (JSONL) data, and a fast HTTP **search server** that serves top-k queries using standard BM25 scoring.

The engine features a highly specialized custom tokenizer built for English, CJK (Chinese, Japanese, Korean) characters, and **legal/statutory texts** (capable of intelligently merging citations like `s.1`, `section 1`, and bracketed structures like `(1)(a)`).

## Features

* **Advanced Tokenization**: Includes Snowball stemming, custom stopword filtering, accent flattening, and punctuation handling.
* **Statutory Citation Parsing**: Automatically detects and merges legal abbreviations and bracketed references.
* **Multilingual Support**: Seamlessly handles UTF-8 CJK text alongside English.
* **Fast BM25 Scoring**: Precomputes Document Frequencies (DF) and loads a highly optimized in-memory inverted index.
* **HTTP API**: Provides a fast, multithreaded REST endpoint for querying via JSONL using `cpp-httplib`.

## Prerequisites & Dependencies

To build this project, you need a C++ compiler that supports **C++23** (e.g., GCC 12+ or Clang 14+).

You will also need to download the following header-only libraries and source files:

1. [**utfcpp**](https://github.com/nemtrif/utfcpp) (`utf8.h`) - For UTF-8 parsing.
2. [**Snowball Stemmer**](https://snowballstem.org/download.html) (`libstemmer_c-3.0.1.tar.gz`) - C stemming library.
3. [**nlohmann/json**](https://github.com/nlohmann/json) (`json.hpp`) - For JSON parsing.
4. [**cpp-httplib**](https://github.com/yhirose/cpp-httplib) (`httplib.h`) - For the HTTP server.
5. [**LexisNexis Stopwords**](https://github.com/igorbrigadir/stopwords/blob/master/en/lexisnexis.txt) - Save this as `lexisnexis_stopwords.txt`.

### Expected Directory Structure
Before building, ensure your project directory looks like this:

```text
.
├── bm25-indexer.cpp
├── bm25-server.cpp
├── bm25-tokenizer.hpp
├── build.bat                  # (Windows build script)
├── build.sh                   # (Linux build script)
├── lexisnexis_stopwords.txt   # Stopwords file in root directory
├── libstemmer_c-3.0.1/        # Extracted Snowball source code
├── include/                   # Create this folder and place headers here:
│   ├── httplib.h
│   ├── utf8.h
│   ├── nlohmann/
│   │   └── json.hpp
│   └── libstemmer.h           # (Copy this from libstemmer_c-3.0.1/include/)
```

## Building

The build scripts will first compile `libstemmer` into a static library (`libstemmer.a`), then compile the indexer and server.

### Windows (MinGW)
Ensure `mingw32-make` and `g++` are in your PATH.
```cmd
build.bat
```

### Linux
Ensure `make` and `g++` are installed.
```bash
chmod +x build.sh
./build.sh
```

## Data Format

### Indexer Input (`input.jsonl`)
The input must be a JSONL file where **each line** is a valid JSON object containing at least an `id` (`uint32_t`) and `text` (UTF-8 string).

```json
{"id": 1, "text": "The quick brown fox jumps over the lazy dog."}
{"id": 2, "text": "According to section 2(1)(a) of the Act, the provision applies."}
```

## Usage

### 1. Run the Indexer
The indexer reads your raw data and generates a document store (`docs.jsonl`) and an inverted index (`index.jsonl`).

**Windows:**
```cmd
indexer.exe -i input.jsonl -o1 docs.jsonl -o2 index.jsonl
```
**Linux:**
```bash
./indexer -i input.jsonl -o1 docs.jsonl -o2 index.jsonl
```

### 2. Start the Server
The server loads the generated index into memory and listens for incoming POST requests.

**Windows:**
```cmd
server.exe -i1 docs.jsonl -i2 index.jsonl -p 8080
```
**Linux:**
```bash
./server -i1 docs.jsonl -i2 index.jsonl -p 8080
```

### 3. Querying the Server
Send a `POST` request to `/` with a JSONL payload containing a `query` string and `k` (the maximum number of results to return).

**Example Request:**
```bash
curl -X POST http://localhost:8080/ \
     -H "Content-Type: application/jsonlines" \
     -d '{"query": "fox jump", "k": 5}'
```

**Example Response:**
```json
[{"id": 1, "score": 1.2845}]
```

*Note: The server supports bulk querying. You can send multiple JSON objects separated by newlines in a single POST request, and the server will stream back the top-k results for each query line-by-line.*

## Credits & Open Source Libraries

This project relies on the following excellent open-source libraries:
* [nlohmann/json](https://github.com/nlohmann/json) (MIT License)
* [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT License)
* [nemtrif/utfcpp](https://github.com/nemtrif/utfcpp) (Boost Software License)
* [Snowball libstemmer](https://github.com/snowballstem/snowball) (BSD-3-Clause)
* [Stopwords collection by igorbrigadir](https://github.com/igorbrigadir/stopwords)