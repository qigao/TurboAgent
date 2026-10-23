#!/usr/bin/env bash
set -euo pipefail

rid="${1:?usage: restore-native-sdk.sh <rid> <with-praktor:0|1> [with-wasm:0|1] [github|json]}"
with_praktor="${2:-0}"
with_wasm="${3:-0}"
output_format="${4:-github}"
case "$rid" in
  windows-x64|linux-x64|macos-arm64|android-arm64-v8a) ;;
  *) echo "unsupported SDK RID: $rid" >&2; exit 2 ;;
esac
if [[ "$with_praktor" != "0" && "$with_praktor" != "1" ]]; then
  echo "with-praktor must be 0 or 1" >&2
  exit 2
fi
if [[ "$with_wasm" != "0" && "$with_wasm" != "1" ]]; then
  echo "with-wasm must be 0 or 1" >&2
  exit 2
fi
if [[ "$output_format" != github && "$output_format" != json ]]; then
  echo "output format must be github or json" >&2
  exit 2
fi
if [[ "$output_format" == github ]]; then
  : "${GITHUB_ENV:?GITHUB_ENV is required for GitHub output}"
fi
windows_host=0
case "$(uname -s)" in
  MINGW*|MSYS*) windows_host=1 ;;
esac
if [[ "$windows_host" == 1 && "$rid" != windows-x64 ]]; then
  echo "Git Bash SDK restore supports windows-x64 only" >&2
  exit 2
fi
if [[ "$rid" == windows-x64 && "$with_praktor" == 1 ]]; then
  echo "published Praktor Native packages do not provide a Windows SDK" >&2
  exit 2
fi
sdk_root="${SDK_RESTORE_ROOT:-}"
if [[ -z "$sdk_root" ]]; then
  sdk_root="${RUNNER_TEMP:?SDK_RESTORE_ROOT or RUNNER_TEMP is required}/turboagent-release-sdks"
fi
# Git Bash uses POSIX paths for filesystem operations; presets need native CMake
# paths. Convert only at these boundaries, leaving Unix paths unchanged.
if [[ "$windows_host" == 1 ]]; then
  sdk_root="$(cygpath -u -- "$sdk_root")"
fi
if [[ "$sdk_root" != /* || "$sdk_root" == / ]]; then
  echo "SDK_RESTORE_ROOT (or RUNNER_TEMP) must specify an absolute restore directory" >&2
  exit 2
fi
mkdir -p "$sdk_root"
sdk_root="$(cd -- "$sdk_root" && pwd -P)"
if [[ "$sdk_root" == / ]]; then
  echo "SDK restore directory must not be the filesystem root" >&2
  exit 2
fi
if [[ "$windows_host" == 1 && "$(cygpath -m -- "$sdk_root")" =~ ^[A-Za-z]:/$ ]]; then
  echo "SDK restore directory must not be a Windows drive root" >&2
  exit 2
fi

# Each dependency is selected by an exact published Release Tag supplied by the
# shared native SDK workflow. Never resolve "latest", git refs, or source SHA.
repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
sdk_manifest="$repository_root/build/release-provenance/$rid.txt"
mkdir -p -- "$(dirname -- "$sdk_manifest")"
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
  local digest

  if [[ ! "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]]; then
    echo "invalid exact release tag for $repo: '$tag'" >&2
    return 1
  fi

  asset="${package_id}.${tag#v}.nupkg"
  printf 'Restoring %s@%s asset %s (%s)\n' "$repo" "$tag" "$asset" "$rid" >&2
  # Package subdirectories are fixed below and stay within the restore root.
  if [[ "$dir" != "$sdk_root/"* || "$name" == */* || "$name" == . || "$name" == .. ]]; then
    echo "SDK restore directory must be directly inside $sdk_root" >&2
    return 1
  fi
  rm -rf -- "$dir" || return 1
  mkdir -p "$dir/download" "$dir/package" || return 1
  python "$repository_root/cmake/ci/github-packages.py" release \
    "$repo" "$tag" "$asset" "$dir/download/$asset" || return 1

  nupkg="$dir/download/$asset"
  if [[ ! -f "$nupkg" ]]; then
    echo "missing exact release asset: $repo@$tag/$asset" >&2
    return 1
  fi
  if command -v sha256sum >/dev/null 2>&1; then
    digest="$(sha256sum "$nupkg" | awk '{print $1}')" || return 1
  else
    digest="$(shasum -a 256 "$nupkg" | awk '{print $1}')" || return 1
  fi
  printf 'repo=%s tag=%s asset=%s sha256=%s\n' \
    "$repo" "$tag" "$asset" "$digest" >> "$sdk_manifest" || return 1
  unzip -q "$nupkg" -d "$dir/package" || return 1

  local root="$dir/package/sdk/$rid"
  if [[ ! -f "$root/$config_rel" ]]; then
    echo "missing installed SDK config for exact release: $root/$config_rel" >&2
    return 1
  fi
  if [[ "$windows_host" == 1 ]]; then
    cygpath -m -- "$root"
  else
    printf '%s\n' "$root"
  fi
}

salts_root="$(restore_sdk qigao/salts Salts.Native salts 'lib/cmake/Salts/SaltsConfig.cmake' "${SALTS_RELEASE_TAG:?SALTS_RELEASE_TAG is required}")"
utils_root="$(restore_sdk qigao/salts-utils SaltsUtils.Native salts-utils 'lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake' "${SALTS_UTILS_RELEASE_TAG:?SALTS_UTILS_RELEASE_TAG is required}")"
chttp_root="$(restore_sdk qigao/chttp CHttp.Native chttp 'lib/cmake/Chttp/ChttpConfig.cmake' "${CHTTP_RELEASE_TAG:?CHTTP_RELEASE_TAG is required}")"

host_idlc=""
host_salts_root=""
turboscript_root=""
praktor_root=""
turbowasm_root=""

if [[ "$rid" == android-* ]]; then
  # Native packages carry the Linux host tools beside the Android target SDK.
  host_salts_root="$(dirname -- "$salts_root")/linux-x64"
  host_utils_root="$(dirname -- "$utils_root")/linux-x64"
  test -f "$host_salts_root/lib/cmake/Salts/SaltsConfig.cmake"
  test -f "$host_utils_root/bin/salts-idlc"
  chmod +x "$host_utils_root/bin/salts-idlc"
  host_idlc="$host_utils_root/bin/salts-idlc"
fi

if [[ "$with_praktor" == "1" ]]; then
  turboscript_root="$(restore_sdk qigao/TurboScript TurboScript.Native turboscript 'lib/cmake/TurboScript/TurboScriptConfig.cmake' "${TURBOSCRIPT_RELEASE_TAG:?TURBOSCRIPT_RELEASE_TAG is required}")"
  praktor_root="$(restore_sdk qigao/praktor Praktor.Native praktor 'lib/cmake/Praktor/PraktorConfig.cmake' "${PRAKTOR_RELEASE_TAG:?PRAKTOR_RELEASE_TAG is required}")"
fi

if [[ "$with_wasm" == "1" ]]; then
  turbowasm_root="$(restore_sdk qigao/turbowasm TurboWasm.Native turbowasm 'lib/cmake/TurboWasm/TurboWasmConfig.cmake' "${TURBOWASM_RELEASE_TAG:?TURBOWASM_RELEASE_TAG is required}")"
fi

# Publish roots only after every requested SDK has been restored successfully.
roots="$(jq -cn --arg salts "$salts_root" --arg utils "$utils_root" \
  --arg chttp "$chttp_root" --arg wasm "$turbowasm_root" \
  --arg script "$turboscript_root" --arg praktor "$praktor_root" \
  --arg idlc "$host_idlc" --arg host_salts "$host_salts_root" \
  '{SALTS_ROOT:$salts,SALTS_UTILS_ROOT:$utils,CHTTP_ROOT:$chttp,TURBOWASM_ROOT:$wasm,
    TURBOSCRIPT_ROOT:$script,PRAKTOR_ROOT:$praktor,TURBOAGENT_HOST_IDLC:$idlc,
    TURBOAGENT_HOST_SALTS_ROOT:$host_salts}')"
if [[ "$output_format" == json ]]; then
  printf '%s\n' "$roots"
else
  jq -r 'to_entries[] | select(.value != "") | "\(.key)=\(.value)"' <<< "$roots" >> "$GITHUB_ENV"
fi
