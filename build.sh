rm -rf build
mkdir build
x86_64-w64-mingw32-gcc -O3 -Wall -Werror -fpic -shared ./src/fzf.c -o ./build/libfzf.dll
