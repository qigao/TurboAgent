param(
  [Parameter(Mandatory = $true)][string]$Rid
)

$ErrorActionPreference = "Stop"
$sdkRoot = Join-Path $env:RUNNER_TEMP "turboagent-release-sdks"
New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null

function Restore-Sdk(
  [string]$Repo,
  [string]$Pattern,
  [string]$Name,
  [string]$ConfigRelative
) {
  $tag = (& gh release view --repo $Repo --json tagName --jq .tagName).Trim()
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($tag)) {
    throw "failed to resolve latest release for $Repo"
  }

  $dir = Join-Path $sdkRoot $Name
  if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
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
  return $root
}

$saltsRoot = Restore-Sdk "qigao/salts" "Salts.Native.*.nupkg" "salts" "lib\cmake\Salts\SaltsConfig.cmake"
$utilsRoot = Restore-Sdk "qigao/salts-utils" "SaltsUtils.Native.*.nupkg" "salts-utils" "lib\cmake\SaltsUtils\SaltsUtilsConfig.cmake"
$chttpRoot = Restore-Sdk "qigao/chttp" "CHttp.Native.*.nupkg" "chttp" "lib\cmake\Chttp\ChttpConfig.cmake"

"SALTS_ROOT=$saltsRoot" >> $env:GITHUB_ENV
"SALTS_UTILS_ROOT=$utilsRoot" >> $env:GITHUB_ENV
"CHTTP_ROOT=$chttpRoot" >> $env:GITHUB_ENV
