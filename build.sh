#!/bin/sh
# Cross-compiles tcsftp.wfx64 and the test harness with mingw-w64.
# Run scripts/build-deps.sh first; DEPS must point at the same prefix.
set -e
DEPS=${DEPS:-$PWD/deps}
CXX=${CXX:-x86_64-w64-mingw32-g++}
mkdir -p build
$CXX -std=c++17 -O2 -Wall -shared -static -static-libgcc -static-libstdc++ \
	-I"$DEPS/win-ossl/include" src/plugin.cpp src/session.cpp src/tcsftp.def \
	-o build/tcsftp.wfx64 -L"$DEPS/win-ossl/lib" -L"$DEPS/ossl/lib" \
	-lssh2 -lcrypto -lws2_32 -lcrypt32 -luser32 -ladvapi32 -lbcrypt -s
$CXX -std=c++17 -O2 -municode -static -o build/harness.exe tests/harness.cpp
cp pluginst.inf README.md LICENSE build/
(cd build && rm -f tcsftp.zip && zip -q tcsftp.zip tcsftp.wfx64 pluginst.inf README.md LICENSE)
ls -la build/tcsftp.wfx64 build/tcsftp.zip
