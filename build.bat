cd libstemmer_c-3.0.1
mingw32-make

:: Create the lib directory if it doesn't exist
if not exist "..\lib" mkdir "..\lib"

:: Copy the file using the Windows 'copy' command
copy libstemmer.a "..\lib\"

cd ..
g++ -O3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow bm25-indexer.cpp lib/libstemmer.a -I include -o indexer.exe
g++ -O3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow bm25-server.cpp lib/libstemmer.a -I include -o server.exe -lws2_32

echo Build complete!
