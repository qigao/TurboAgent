#!/usr/bin/env bash
set -euo pipefail

rid="${1:?usage: restore-native-sdk.sh <rid> <with-praktor:0|1>}"
with_praktor="${2:-0}"
sdk_root="${RUNNER_TEMP:?RUNNER_TEMP is required}/turboagent-release-sdks"
mkdir -p "$sdk_root"

restore_sdk() {
  local repo="$1"
  local pattern="$2"
  local name="$3"
  local config_rel="$4"
  local dir="$sdk_root/$name"
  local tag
  local nupkg

  tag="$(gh release view --repo "$repo" --json tagName --jq .tagName)"
  test -n "$tag"
  rm -rf "$dir"
  mkdir -p "$dir/download" "$dir/package"
  gh release download "$tag" --repo "$repo" --pattern "$pattern"     --dir "$dir/download" --clobber
  nupkg="$(find "$dir/download" -maxdepth 1 -type f -name '*.nupkg' -print -quit)"
  test -n "$nupkg"
  unzip -q "$nupkg" -d "$dir/package"
  local root="$dir/package/sdk/$rid"
  test -f "$root/$config_rel"
  printf '%s
' "$root"
}

salts_root="$(restore_sdk qigao/salts 'Salts.Native.*.nupkg' salts 'lib/cmake/Salts/SaltsConfig.cmake')"
utils_root="$(restore_sdk qigao/salts-utils 'SaltsUtils.Native.*.nupkg' salts-utils 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake')"
chttp_root="$(restore_sdk qigao/chttp 'CHttp.Native.*.nupkg' chttp 'lib/cmake/Chttp/ChttpConfig.cmake')"

{
  echo "SALTS_ROOT=$salts_root"
  echo "SALTS_UTILS_ROOT=$utils_root"
  echo "CHTTP_ROOT=$chttp_root"
} >> "$GITHUB_ENV"

if [[ "$with_praktor" == "1" ]]; then
  turboscript_root="$(restore_sdk qigao/TurboScript 'TurboScript.Native.*.nupkg' turboscript 'lib/cmake/TurboScript/TurboScriptConfig.cmake')"
  praktor_root="$(restore_sdk qigao/praktor 'Praktor.Native.*.nupkg' praktor 'lib/cmake/Praktor/PraktorConfig.cmake')"
  {
    echo "TURBOSCRIPT_ROOT=$turboscript_root"
    echo "PRAKTOR_ROOT=$praktor_root"
  } >> "$GITHUB_ENV"
fi
