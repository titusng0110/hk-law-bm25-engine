```
cd libstemmer_c-3.0.1/ && \
    make && \
    mkdir -p ../lib && \
    cp ./libstemmer.a ../lib/
g++ -O3 -std=c++23 -Wall -Wextra -Wpedantic -Wshadow bm25-indexer.cpp lib/libstemmer.a -I include -o indexer.exe
```

Please see:

https://github.com/nemtrif/utfcpp

https://github.com/snowballstem/snowball

https://snowballstem.org/download.html

https://github.com/igorbrigadir/stopwords/blob/master/en/lexisnexis.txt

https://github.com/nlohmann/json

https://github.com/yhirose/cpp-httplib