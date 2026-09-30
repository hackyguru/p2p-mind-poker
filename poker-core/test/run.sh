#!/usr/bin/env bash
# Offline check of the parts that are easy to get subtly wrong: the SRA
# mental-poker pipeline, the betting state machine and the 5-of-7 evaluator.
# poker_crypto.cpp and poker_game.cpp are deliberately Qt-free, so this links
# against nothing but OpenSSL — no Basecamp, no delivery_module, no nix build.
#
#   ./run.sh            # uses an OpenSSL from the nix store
#   OPENSSL_DIR=/opt/homebrew/opt/openssl@3 ./run.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$HERE/.."

if [ -n "${OPENSSL_DIR:-}" ]; then
    INC="$OPENSSL_DIR/include"; LIB="$OPENSSL_DIR/lib"
else
    INC="$(ls -d /nix/store/*openssl-3.*-dev/include 2>/dev/null | tail -1)"
    LIB="$(ls -d /nix/store/*openssl-3.*/lib 2>/dev/null | grep -v -- -dev | tail -1)"
fi
[ -d "$INC" ] && [ -d "$LIB" ] || { echo "set OPENSSL_DIR to an OpenSSL 3 prefix" >&2; exit 1; }

OUT="$(mktemp -d)/poker_harness"
clang++ -std=c++17 -O1 -o "$OUT" \
    "$HERE/poker_harness.cpp" "$SRC/src/poker_crypto.cpp" "$SRC/src/poker_game.cpp" \
    -I"$SRC/src" -I"$INC" -L"$LIB" -lcrypto -Wl,-rpath,"$LIB"
exec "$OUT"
