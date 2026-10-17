#!/bin/sh
# Builds static OpenSSL and libssh2 for 64-bit Windows with mingw-w64 into $DEPS.
set -e
DEPS=${DEPS:-$PWD/deps}
OPENSSL=3.0.15
LIBSSH2=1.11.1
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DEPS/src" && cd "$DEPS/src"
[ -d openssl-$OPENSSL ] || curl -sSL https://github.com/openssl/openssl/releases/download/openssl-$OPENSSL/openssl-$OPENSSL.tar.gz | tar xz
[ -d libssh2-$LIBSSH2 ] || curl -sSL https://github.com/libssh2/libssh2/releases/download/libssh2-$LIBSSH2/libssh2-$LIBSSH2.tar.gz | tar xz
if [ ! -f "$DEPS/ossl/lib/libcrypto.a" ]; then
	(cd openssl-$OPENSSL && ./Configure mingw64 no-shared no-tests no-module no-engine \
		--cross-compile-prefix=x86_64-w64-mingw32- --prefix="$DEPS/ossl" --libdir=lib >/dev/null &&
		make -j"$(nproc)" build_libs >/dev/null && make install_dev >/dev/null)
fi
if [ ! -f "$DEPS/win-ossl/lib/libssh2.a" ]; then
	cmake -S libssh2-$LIBSSH2 -B libssh2-build -DCMAKE_TOOLCHAIN_FILE="$HERE/mingw64.cmake" \
		-DCRYPTO_BACKEND=OpenSSL -DOPENSSL_ROOT_DIR="$DEPS/ossl" -DOPENSSL_USE_STATIC_LIBS=ON \
		-DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF \
		-DENABLE_ZLIB_COMPRESSION=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS/win-ossl" >/dev/null
	cmake --build libssh2-build -j"$(nproc)" >/dev/null
	cmake --install libssh2-build >/dev/null
fi
echo "deps ready in $DEPS"
