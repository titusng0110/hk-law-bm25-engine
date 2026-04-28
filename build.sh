cd libstemmer_c-3.0.1
make
mkdir -p ../lib
cp ./libstemmer.a ../lib/
cd ..
g++ -O3 -std=c++2b -Wall -Wextra -Wpedantic -Wshadow bm25-indexer.cpp lib/libstemmer.a -I include -o indexer
g++ -O3 -std=c++2b -Wall -Wextra -Wpedantic -Wshadow bm25-server.cpp lib/libstemmer.a -I include -o server -pthread
echo Build complete!
