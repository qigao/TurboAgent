#!/usr/bin/env bash
# TEST-ONLY ELF symbol audit. Does not alter or bypass the original linker.
set -euo pipefail
test "$(uname -s)" = Linux
command -v readelf >/dev/null
command -v nm >/dev/null
command -v ar >/dev/null
build_root="$1"
test -d "$build_root/bin"
: "$SALTS_ROOT" "$SALTS_UTILS_ROOT" "$PRAKTOR_ROOT" "$TURBOSCRIPT_ROOT" "$CHTTP_ROOT"
archive="$SALTS_ROOT/lib/libcmeta_cstl.a"
test -f "$archive"
printf 'AUDIT: hidden hash_bytes archive provider: %s\n' "$archive"
nm -A "$archive" | grep -E 'hash_bytes$' || true
temporary="$(mktemp -d "$RUNNER_TEMP/hidden-hash-XXXXXX")"
chmod 700 "$temporary"
trap 'rm -rf -- "$temporary"' EXIT
if ar p "$archive" hash_map.c.o > "$temporary/hash_map.o" && test -s "$temporary/hash_map.o"; then
  readelf --wide --symbols "$temporary/hash_map.o" | grep -E 'hash_bytes([[:space:]]|$)' || true
fi
found=0
scanned=0
for libdir in "$build_root/bin" "$SALTS_ROOT/lib" "$SALTS_UTILS_ROOT/lib" "$PRAKTOR_ROOT/lib" "$TURBOSCRIPT_ROOT/lib" "$CHTTP_ROOT/lib"; do
  test -d "$libdir" || continue
  while IFS= read -r -d '' soname; do
    scanned=$((scanned+1))
    imports="$(nm -D --undefined-only "$soname" 2>/dev/null | grep -E '[[:space:]]U[[:space:]]+hash_bytes(@|$)' || true)"
    if test -n "$imports"; then
      found=$((found+1))
      printf 'AUDIT: DSO HASH_BYTES UNDEFINED %s\n%s\n' "$soname" "$imports"
      readelf --wide --dyn-syms "$soname" | grep -E 'hash_bytes(@|[[:space:]]|$)' || true
      printf 'AUDIT: requesting DSO dependencies:\n'
      readelf --wide --dynamic "$soname" | grep '(NEEDED)' || true
    fi
  done < <(find -L "$libdir" -maxdepth 1 -type f \( -name '*.so' -o -name '*.so.*' \) -print0)
done
printf 'AUDIT: scanned=%s DSOs_with_hash_bytes_undefined=%s\n' "$scanned" "$found"
printf 'AUDIT: main linker failure remains authoritative; no flags changed\n'
