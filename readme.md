To compile:

```
mkdir lib
cd libstemmer_c-3.0.1/
make
cp ./libstemmer.a ../lib/
cd ..
g++ -O3 -std=c++23 main.cpp lib/libstemmer.a -I include -o engine.exe
```

Please see:

https://github.com/nemtrif/utfcpp

https://github.com/snowballstem/snowball

https://snowballstem.org/download.html

https://github.com/igorbrigadir/stopwords/blob/master/en/lexisnexis.txt
