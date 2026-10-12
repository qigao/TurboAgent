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
repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
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

  # The published CHttp RPC Owner ABI requires one immutable Salts/Utils pair.
  # Dynamic release selection is limited to unrelated tool SDKs.
  case "$repo" in
    qigao/salts)
      tag="v2.3.0-rc.15"
      pattern="Salts.Native.2.3.0-rc.15.nupkg"
      ;;
    qigao/salts-utils)
      tag="v4.3.0-rc.10"
      pattern="SaltsUtils.Native.4.3.0-rc.10.nupkg"
      ;;
    qigao/chttp)
      tag="v2.1.0-rc.5"
      pattern="CHttp.Native.2.1.0-rc.5.nupkg"
      ;;
    *)
      if ! tag="$(gh api "repos/$repo/releases?per_page=100" --paginate --slurp |
          jq -er '[.[][] | select(.draft == false and .published_at != null)] | max_by(.published_at) | .tag_name // empty')"; then
        echo "failed to resolve latest published release for $repo" >&2
        return 1
      fi
      ;;
  esac
  printf 'Restoring %s@%s (%s)\n' "$repo" "$tag" "$rid" >&2
  rm -rf "$dir" || return 1
  mkdir -p "$dir/download" "$dir/package" || return 1
  gh release download "$tag" --repo "$repo" --pattern "$pattern" \
    --dir "$dir/download" --clobber || return 1
  nupkg="$(find "$dir/download" -maxdepth 1 -type f -name '*.nupkg' -print -quit)" || return 1
  if [[ -z "$nupkg" ]]; then
    echo "missing downloaded Native package for $repo@$tag" >&2
    return 1
  fi
  unzip -q "$nupkg" -d "$dir/package" || return 1
  case "$repo" in
    qigao/salts|qigao/salts-utils|qigao/chttp)
      python "$repository_root/cmake/ci/verify-published-native-sdk.py" \
        --repo "$repo" --archive "$nupkg" \
        --extracted "$dir/package" --rid "$rid" >&2 || return 1
      ;;
  esac
  local root="$dir/package/sdk/$rid"
  if [[ ! -f "$root/$config_rel" ]]; then
    echo "missing restored SDK config: $root/$config_rel" >&2
    return 1
  fi
  printf '%s\n' "$root"
}

salts_root="$(restore_sdk qigao/salts 'Salts.Native.*.nupkg' salts 'lib/cmake/Salts/SaltsConfig.cmake')"
utils_root="$(restore_sdk qigao/salts-utils 'SaltsUtils.Native.*.nupkg' salts-utils 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake')"
chttp_root="$(restore_sdk qigao/chttp 'CHttp.Native.*.nupkg' chttp 'lib/cmake/Chttp/ChttpConfig.cmake')"

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
  turboscript_root="$(restore_sdk qigao/TurboScript 'TurboScript.Native.*.nupkg' turboscript 'lib/cmake/TurboScript/TurboScriptConfig.cmake')"
  praktor_root="$(restore_sdk qigao/praktor 'Praktor.Native.*.nupkg' praktor 'lib/cmake/Praktor/PraktorConfig.cmake')"
  {
    echo "TURBOSCRIPT_ROOT=$turboscript_root"
    echo "PRAKTOR_ROOT=$praktor_root"
  } >> "$GITHUB_ENV"
fi

if [[ "$with_wasm" == "1" ]]; then
  turbowasm_root="$(restore_sdk qigao/turbowasm 'TurboWasm.Native.*.nupkg' turbowasm 'lib/cmake/TurboWasm/TurboWasmConfig.cmake')"
  echo "TURBOWASM_ROOT=$turbowasm_root" >> "$GITHUB_ENV"
fi
