#!/usr/bin/env bash
set -euo pipefail

rid="${1:?usage: restore-native-sdk.sh <rid> <with-praktor:0|1> [with-wasm:0|1]}"
with_praktor="${2:-0}"
with_wasm="${3:-0}"
if [[ "$with_praktor" != "0" && "$with_praktor" != "1" ]]; then
  echo "with-praktor must be 0 or 1" >&2
  exit 2
fi
if [[ "$with_wasm" != "0" && "$with_wasm" != "1" ]]; then
  echo "with-wasm must be 0 or 1" >&2
  exit 2
fi
sdk_root="${RUNNER_TEMP:?RUNNER_TEMP is required}/turboagent-release-sdks"
mkdir -p "$sdk_root"

# Each dependency is selected by an exact published Release Tag supplied by the
# shared native SDK workflow. Never resolve "latest", git refs, or source SHA.
sdk_manifest="$sdk_root/native-sdk-release-manifest.txt"
: > "$sdk_manifest"

restore_sdk() {
  local repo="$1"
  local package_id="$2"
  local name="$3"
  local config_rel="$4"
  local tag="$5"
  local dir="$sdk_root/$name"
  local asset
  local nupkg
  local published
  local digest

  if [[ ! "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]]; then
    echo "invalid exact release tag for $repo: '$tag'" >&2
    return 1
  fi
  if ! published="$(gh api "repos/$repo/releases/tags/$tag" \
      --jq 'if (.draft == false and .published_at != null) then "yes" else "no" end')"; then
    echo "published Release Tag not found: $repo@$tag" >&2
    return 1
  fi
  if [[ "$published" != "yes" ]]; then
    echo "release is draft or unpublished: $repo@$tag" >&2
    return 1
  fi

  asset="${package_id}.${tag#v}.nupkg"
  printf 'Restoring %s@%s asset %s (%s)\n' "$repo" "$tag" "$asset" "$rid" >&2
  rm -rf -- "$dir"
  mkdir -p "$dir/download" "$dir/package"
  gh release download "$tag" --repo "$repo" --pattern "$asset" \
    --dir "$dir/download" --clobber

  nupkg="$dir/download/$asset"
  if [[ ! -f "$nupkg" ]]; then
    echo "missing exact release asset: $repo@$tag/$asset" >&2
    return 1
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    digest="$(sha256sum "$nupkg" | awk '{print $1}')"
  else
    digest="$(shasum -a 256 "$nupkg" | awk '{print $1}')"
  fi
  printf 'repo=%s tag=%s asset=%s sha256=%s\n' \
    "$repo" "$tag" "$asset" "$digest" >> "$sdk_manifest"
  unzip -q "$nupkg" -d "$dir/package"

  local root="$dir/package/sdk/$rid"
  if [[ ! -f "$root/$config_rel" ]]; then
    echo "missing installed SDK config for exact release: $root/$config_rel" >&2
    return 1
  fi
  printf '%s\n' "$root"
}

salts_root="$(restore_sdk qigao/salts Salts.Native salts 'lib/cmake/Salts/SaltsConfig.cmake' "${SALTS_RELEASE_TAG:?SALTS_RELEASE_TAG is required}")"
utils_root="$(restore_sdk qigao/salts-utils SaltsUtils.Native salts-utils 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake' "${SALTS_UTILS_RELEASE_TAG:?SALTS_UTILS_RELEASE_TAG is required}")"
chttp_root="$(restore_sdk qigao/chttp CHttp.Native chttp 'lib/cmake/Chttp/ChttpConfig.cmake' "${CHTTP_RELEASE_TAG:?CHTTP_RELEASE_TAG is required}")"

{
  echo "SALTS_ROOT=$salts_root"
  echo "SALTS_UTILS_ROOT=$utils_root"
  echo "CHTTP_ROOT=$chttp_root"
} >> "$GITHUB_ENV"

if [[ "$rid" == android-* ]]; then
  # Native packages carry the Linux host tools beside the Android target SDK.
  host_salts_root="$(dirname -- "$salts_root")/linux-x64"
  host_utils_root="$(dirname -- "$utils_root")/linux-x64"
  test -f "$host_salts_root/lib/cmake/Salts/SaltsConfig.cmake"
  test -f "$host_utils_root/bin/salts-idlc"
  chmod +x "$host_utils_root/bin/salts-idlc"
  {
    echo "TURBOAGENT_HOST_IDLC=$host_utils_root/bin/salts-idlc"
    echo "TURBOAGENT_HOST_SALTS_ROOT=$host_salts_root"
  } >> "$GITHUB_ENV"
fi

if [[ "$with_praktor" == "1" ]]; then
  turboscript_root="$(restore_sdk qigao/TurboScript TurboScript.Native turboscript 'lib/cmake/TurboScript/TurboScriptConfig.cmake' "${TURBOSCRIPT_RELEASE_TAG:?TURBOSCRIPT_RELEASE_TAG is required}")"
  praktor_root="$(restore_sdk qigao/praktor Praktor.Native praktor 'lib/cmake/Praktor/PraktorConfig.cmake' "${PRAKTOR_RELEASE_TAG:?PRAKTOR_RELEASE_TAG is required}")"
  {
    echo "TURBOSCRIPT_ROOT=$turboscript_root"
    echo "PRAKTOR_ROOT=$praktor_root"
  } >> "$GITHUB_ENV"
fi

if [[ "$with_wasm" == "1" ]]; then
  turbowasm_root="$(restore_sdk qigao/turbowasm TurboWasm.Native turbowasm 'lib/cmake/TurboWasm/TurboWasmConfig.cmake' "${TURBOWASM_RELEASE_TAG:?TURBOWASM_RELEASE_TAG is required}")"
  echo "TURBOWASM_ROOT=$turbowasm_root" >> "$GITHUB_ENV"
fi
