#!/usr/bin/env bash
# Diagnostic only: identify a DSO's unresolved CSTL symbol before changing ownership.
# See qigao/praktor#73 and qigao/TurboAgent#71.
set -euo pipefail

sdk_root="${RUNNER_TEMP:?RUNNER_TEMP is required}/turboagent-release-sdks"
report="${PRAKTOR_CSTL_REPORT_DIR:-${RUNNER_TEMP}/praktor-cstl-owner-report}"
mkdir -p "$report"
: > "$report/undefined-hash-by-image.txt"
: > "$report/cstl-static-symbols.txt"
: > "$report/restored-native-packages.sha256"

for pkg in "$sdk_root"/*/download/*.nupkg; do
  [[ -f "$pkg" ]] || continue
  sha256sum "$pkg" >> "$report/restored-native-packages.sha256"
done
test -s "$report/restored-native-packages.sha256"

salts_archive="$sdk_root/salts/package/sdk/linux-x64/lib/libcmeta_cstl.a"
test -f "$salts_archive"
nm -A --defined-only "$salts_archive" |
  grep -E '[[:space:]][A-Za-z][[:space:]]hash_bytes$' |
  tee "$report/cstl-static-symbols.txt" || true
readelf -Ws "$salts_archive" > "$report/cstl-static-elf-symbols.txt"
grep -E '[[:space:]]hash_bytes$' "$report/cstl-static-elf-symbols.txt" |
  tee "$report/cstl-static-hash-elf.txt" || true
test -s "$report/cstl-static-symbols.txt"
test -s "$report/cstl-static-hash-elf.txt"

# Scan every actual ELF shared object, not just libPraktor.so. The link error
# says "referenced by DSO" but cannot identify that DSO by itself.
images=0
offenders=0
while IFS= read -r -d '' lib; do
  if ! readelf -h "$lib" >/dev/null 2>&1; then
    continue
  fi
  images=$((images + 1))
  if nm -D --undefined-only "$lib" | grep -E '[[:space:]](U|w)[[:space:]]hash_bytes(@|$)' > "$report/current-symbol.txt"; then
    offenders=$((offenders + 1))
    {
      printf 'DSO=%s\n' "$lib"
      cat "$report/current-symbol.txt"
      printf '\n'
    } | tee -a "$report/undefined-hash-by-image.txt"
    short="$(basename "$lib")"
    readelf -Ws "$lib" > "$report/${short}.${offenders}.symbols.txt"
    readelf -d "$lib" > "$report/${short}.${offenders}.needed.txt"
    nm -D --undefined-only "$lib" > "$report/${short}.${offenders}.undefined.txt"
  fi
done < <(find "$sdk_root" -type f -name '*.so*' -print0)

printf 'scanned_ELF_shared_images=%s\n' "$images" | tee "$report/summary.txt"
printf 'undefined_hash_bytes_DSO_count=%s\n' "$offenders" | tee -a "$report/summary.txt"
printf 'symbol_policy=Salts_static_hidden_image_local; do_not_globally_export_hash_bytes\n' |
  tee -a "$report/summary.txt"
test "$images" -gt 0

# Do not prematurely assume the offending DSO is Praktor; keep the link
# reproduction below as the actual fail-closed acceptance.
if (( offenders == 0 )); then
  echo 'No direct DSO hash_bytes import found; inspect linked import/DT_NEEDED chain and final linker command.'
fi
