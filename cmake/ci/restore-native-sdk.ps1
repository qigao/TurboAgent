param(
  [Parameter(Mandatory = $true)][string]$Rid,
  [switch]$WithWasm
)

$ErrorActionPreference = "Stop"
$versions = Get-Content -LiteralPath (Join-Path $PSScriptRoot "native-sdk-versions.json") -Raw | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) { throw "RUNNER_TEMP is required" }
$sdkRoot = [System.IO.Path]::GetFullPath((Join-Path $env:RUNNER_TEMP "turboagent-release-sdks"))
New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null

function Restore-Sdk(
  [string]$Repo,
  [string]$Pattern,
  [string]$Name,
  [string]$ConfigRelative
) {
  $tag = $versions.$Repo
  if ([string]::IsNullOrWhiteSpace($tag)) { throw "missing SDK release selection for $Repo" }
  if ($tag -eq "latest") {
    $tag = (& gh release view --repo $Repo --json tagName --jq .tagName).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($tag)) {
      throw "failed to resolve latest release for $Repo"
    }
  }

  $dir = [System.IO.Path]::GetFullPath((Join-Path $sdkRoot $Name))
  if ([System.IO.Path]::GetDirectoryName($dir) -ne $sdkRoot) {
    throw "SDK restore directory must be directly inside $sdkRoot"
  }
  if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
  $download = Join-Path $dir "download"
  $package = Join-Path $dir "package"
  New-Item -ItemType Directory -Force -Path $download, $package | Out-Null

  gh release download $tag --repo $Repo --pattern $Pattern --dir $download --clobber
  if ($LASTEXITCODE -ne 0) {
    throw "failed to download $Pattern from $Repo@$tag"
  }

  $nupkg = Get-ChildItem -Path $download -File -Filter *.nupkg | Select-Object -First 1
  if (-not $nupkg) { throw "missing downloaded Native package for $Repo@$tag" }

  $zip = "$($nupkg.FullName).zip"
  Copy-Item -LiteralPath $nupkg.FullName -Destination $zip -Force
  Expand-Archive -LiteralPath $zip -DestinationPath $package -Force

  $root = Join-Path $package ("sdk\" + $Rid)
  $config = Join-Path $root $ConfigRelative
  if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
    throw "missing restored SDK config: $config"
  }
  # These roots feed CMake presets and must use CMake path separators.
  return $root.Replace('\', '/')
}

$saltsRoot = Restore-Sdk "qigao/salts" "Salts.Native.*.nupkg" "salts" "lib\cmake\Salts\SaltsConfig.cmake"
$utilsRoot = Restore-Sdk "qigao/salts-utils" "SaltsUtils.Native.*.nupkg" "salts-utils" "lib\cmake\SaltsUtils\SaltsUtilsConfig.cmake"
$chttpRoot = Restore-Sdk "qigao/chttp" "CHttp.Native.*.nupkg" "chttp" "lib\cmake\Chttp\ChttpConfig.cmake"

"SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
"SALTS_UTILS_ROOT=$utilsRoot" >> $env:GITHUB_ENV
"CHTTP_ROOT=$chttpRoot" >> $env:GITHUB_ENV

if ($WithWasm) {
  $turbowasmRoot = Restore-Sdk "qigao/turbowasm" "TurboWasm.Native.*.nupkg" "turbowasm" "lib\cmake\TurboWasm\TurboWasmConfig.cmake"
  "TURBOWASM_ROOT=$turbowasmRoot" >> $env:GITHUB_ENV
}
