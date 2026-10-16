param(
  [Parameter(Mandatory = $true)][string]$Rid,
  [switch]$WithWasm
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) { throw "RUNNER_TEMP is required" }
$sdkRoot = [System.IO.Path]::GetFullPath((Join-Path $env:RUNNER_TEMP "turboagent-release-sdks"))
New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null

# The workflow supplies exact published tags. Do not query "latest" or use
# a source checkout, Git SHA, or fallback package for a release-SDK consumer.
if ([string]::IsNullOrWhiteSpace($env:GITHUB_WORKSPACE)) { throw "GITHUB_WORKSPACE is required" }
$sdkManifest = Join-Path $env:GITHUB_WORKSPACE "build/release-provenance/$Rid.txt"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sdkManifest) | Out-Null
[IO.File]::WriteAllText($sdkManifest, "", [Text.UTF8Encoding]::new($false))

function Restore-Sdk(
  [string]$Repo,
  [string]$PackageId,
  [string]$Name,
  [string]$ConfigRelative,
  [string]$Tag
) {
  if ([string]::IsNullOrWhiteSpace($Tag) -or
      $Tag -cnotmatch '^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$') {
    throw "invalid exact Release Tag for $Repo : '$Tag'"
  }
  $metadata = & gh api "repos/$Repo/releases/tags/$Tag"
  if ($LASTEXITCODE -ne 0) {
    throw "published Release Tag not found: $Repo@$Tag"
  }
  $release = ($metadata -join "`n") | ConvertFrom-Json
  if ($release.draft -or -not $release.published_at -or $release.tag_name -cne $Tag) {
    throw "release is draft, unpublished or mismatched: $Repo@$Tag"
  }

  $asset = "$PackageId.$($Tag.Substring(1)).nupkg"
  [Console]::Error.WriteLine("Restoring $Repo@$Tag asset $asset ($Rid)")
  $dir = [System.IO.Path]::GetFullPath((Join-Path $sdkRoot $Name))
  if ([System.IO.Path]::GetDirectoryName($dir) -ne $sdkRoot) {
    throw "SDK restore directory must be directly inside $sdkRoot"
  }
  if (Test-Path -LiteralPath $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }
  $download = Join-Path $dir "download"
  $package = Join-Path $dir "package"
  New-Item -ItemType Directory -Force -Path $download, $package | Out-Null

  gh release download $Tag --repo $Repo --pattern $asset --dir $download --clobber
  if ($LASTEXITCODE -ne 0) {
    throw "failed to download exact release asset: $Repo@$Tag/$asset"
  }
  $nupkg = Join-Path $download $asset
  if (-not (Test-Path -LiteralPath $nupkg -PathType Leaf)) {
    throw "missing exact release asset: $Repo@$Tag/$asset"
  }
  $digest = (Get-FileHash -LiteralPath $nupkg -Algorithm SHA256).Hash.ToLowerInvariant()
  Add-Content -LiteralPath $sdkManifest -Encoding utf8 -Value "repo=$Repo tag=$Tag asset=$asset sha256=$digest"

  $zip = "$nupkg.zip"
  Copy-Item -LiteralPath $nupkg -Destination $zip -Force
  Expand-Archive -LiteralPath $zip -DestinationPath $package -Force

  $root = Join-Path $package ("sdk\" + $Rid)
  $config = Join-Path $root $ConfigRelative
  if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
    throw "missing restored SDK config: $config"
  }
  return $root.Replace('\', '/')
}

$saltsRoot = Restore-Sdk "qigao/salts" "Salts.Native" "salts" "lib\cmake\Salts\SaltsConfig.cmake" $env:SALTS_RELEASE_TAG
$utilsRoot = Restore-Sdk "qigao/salts-utils" "SaltsUtils.Native" "salts-utils" "lib\cmake\SaltsUtils\SaltsUtilsConfig.cmake" $env:SALTS_UTILS_RELEASE_TAG
$chttpRoot = Restore-Sdk "qigao/chttp" "CHttp.Native" "chttp" "lib\cmake\Chttp\ChttpConfig.cmake" $env:CHTTP_RELEASE_TAG

"SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
"SALTS_UTILS_ROOT=$utilsRoot" >> $env:GITHUB_ENV
"CHTTP_ROOT=$chttpRoot" >> $env:GITHUB_ENV

if ($WithWasm) {
  $turbowasmRoot = Restore-Sdk "qigao/turbowasm" "TurboWasm.Native" "turbowasm" "lib\cmake\TurboWasm\TurboWasmConfig.cmake" $env:TURBOWASM_RELEASE_TAG
  "TURBOWASM_ROOT=$turbowasmRoot" >> $env:GITHUB_ENV
}
