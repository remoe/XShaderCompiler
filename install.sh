#!/bin/sh

rm -rf install
mkdir -p install/lib

mkdir Build
cd Build

cmake -G "Unix Makefiles" -DINSTALL_OUTPUT_PATH="$PWD/../install" ../
cmake --build . --config Release
cmake --build . --config Release --target install

cd ..
mv "$PWD/install/libxsc_core.a" "$PWD/install/lib/libxsc_core.a"

rm -rf Build
