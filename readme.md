# Hong Kong Law BM25 Search Engine

A high-performance, lightweight BM25 search engine written in C++23, designed specifically for **Hong Kong law and legal documents** (ordinances, case law, contracts, etc.).

It consists of an offline **indexer** that builds an inverted index from JSON Lines (JSONL) data, and a fast HTTP **search server** that serves top-k queries using standard BM25 scoring.

The engine features a highly specialized custom tokenizer built to handle the unique quirks of HK legal text, including English, CJK (Chinese, Japanese, Korean) characters, and **statutory texts** (capable of intelligently merging citations like `s.1`, `section 1`, `Cap. 4`, and bracketed structures like `(1)(a)` or `Sch 1`).

## Features

* **Tailored for Legal Text**: Automatically detects and merges legal abbreviations, statutory citations, and bracketed references common in HK jurisprudence.
* **Multilingual Support**: Seamlessly handles UTF-8 CJK text alongside English, perfect for bilingual HK legal documents.
* **Advanced Tokenization**: Includes Snowball stemming, custom stopword filtering, accent flattening, and punctuation handling.
* **Fast BM25 Scoring**: Precomputes Document Frequencies (DF) and loads a highly optimized in-memory inverted index.
* **HTTP API**: Provides a fast, multithreaded REST endpoint for querying via JSONL using `cpp-httplib`.

## Prerequisites

To build this project, you only need a C++ compiler that supports **C++23** (e.g., GCC 12+ or Clang 14+) and `make`.

> **🪟 Windows Users:** We highly recommend downloading a modern GCC/MinGW-w64 build from [niXman/mingw-builds-binaries](https://github.com/niXman/mingw-builds-binaries/) to ensure proper C++23 support. Ensure `g++` and `mingw32-make` are added to your system's PATH.

**Everything else is already included!** All third-party headers, the Snowball stemmer source (`libstemmer_c-3.0.1`), and the LexisNexis stopwords file are bundled directly in this repository. You do not need to download any external dependencies.

## Building

The build scripts will automatically compile `libstemmer` into a static library (`libstemmer.a`), then compile the indexer and server.

### Windows (MinGW)
Open your command prompt or terminal in the project directory and run:
```cmd
build.bat
```

### Linux
Open your terminal in the project directory and run:
```bash
chmod u+x build.sh
./build.sh
```

## Data Format

### Indexer Input (`input.jsonl`)
The input must be a JSONL file where **each line** is a valid JSON object containing at least an `id` (`uint32_t`) and `text` (UTF-8 string).

```json
{"id": 1, "text": "The quick brown fox jumps over the lazy dog."}
{"id": 2, "text": "According to section 2(1)(a) of the Employment Ordinance (Cap. 57), the provision applies."}
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
     -d '{"query": "employment cap 57 section 2", "k": 5}'
```

**Example Response:**
```json
[{"id": 2, "score": 4.1845}]
```

*Note: The server supports bulk querying. You can send multiple JSON objects separated by newlines in a single POST request, and the server will stream back the top-k results for each query line-by-line.*

## Credits & Open Source Libraries

This project bundles and relies on the following excellent open-source libraries:
* [nlohmann/json](https://github.com/nlohmann/json) (MIT License)
* [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) (MIT License)
* [nemtrif/utfcpp](https://github.com/nemtrif/utfcpp) (Boost Software License)
* [Snowball libstemmer](https://github.com/snowballstem/snowball) (BSD-3-Clause)
* [Stopwords collection by igorbrigadir](https://github.com/igorbrigadir/stopwords)