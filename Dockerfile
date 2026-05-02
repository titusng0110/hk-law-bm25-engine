# ==========================================
# STAGE 1: Build Binaries and Generate Index
# ==========================================
FROM alpine:latest AS builder

# Install Alpine's C++ toolchain (musl-based)
RUN apk add --no-cache g++ make

WORKDIR /app

# Copy all your source files, headers, and input data
COPY . .

# 1. Build libstemmer
RUN cd libstemmer_c-3.0.1 && \
    make && \
    mkdir -p ../lib && \
    cp ./libstemmer.a ../lib/

# 2. Build indexer and server
# Note: httplib in Linux needs -pthread
RUN g++ -O3 -std=c++2b -Wall -Wextra -Wpedantic -Wshadow bm25-indexer.cpp lib/libstemmer.a -I include -o indexer && \
    g++ -O3 -std=c++2b -Wall -Wextra -Wpedantic -Wshadow bm25-server.cpp lib/libstemmer.a -I include -o server -pthread

# 3. Run the indexer inside the builder stage!
# This bakes input.jsonl into docs.jsonl and index.bin
RUN ./indexer -i input.jsonl -o1 docs.jsonl -o2 index.bin


# ==========================================
# STAGE 2: Minimal Runtime Container
# ==========================================
FROM alpine:latest

# We only need the C++ standard library runtime, not the whole compiler
RUN apk add --no-cache libstdc++

WORKDIR /app

# Copy ONLY the compiled server and the required data files from the builder
COPY --from=builder /app/server .
COPY --from=builder /app/docs.jsonl .
COPY --from=builder /app/index.bin .
COPY --from=builder /app/lexisnexis_stopwords.txt .

# Expose the port
EXPOSE 8080

# Run the server
CMD ["./server", "-i1", "docs.jsonl", "-i2", "index.bin", "-p", "8080"]
