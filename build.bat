cd libstemmer_c-3.0.1
mingw32-make
if not exist "..\lib" mkdir "..\lib"
copy libstemmer.a "..\lib\"
cd ..
g++ -O3 -std=c++2b -Wall -Wextra -Wpedantic -Wshadow bm25-indexer.cpp lib/libstemmer.a -I include -o indexer.exe
g++ -O3 -std=c++2b -Wall -Wextra -Wpedantic -Wshadow bm25-server.cpp lib/libstemmer.a -I include -o server.exe -lws2_32
echo Build complete!
