param(
  [Parameter(Mandatory = $true)][string]$Rid,
  [switch]$WithWasm
)

$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) { throw "RUNNER_TEMP is required" }
$sdkRoot = [System.IO.Path]::GetFullPath((Join-Path $env:RUNNER_TEMP "turboagent-release-sdks"))
New-Item -ItemType Directory -Force -Path $sdkRoot | Out-Null

function Restore-Sdk(
  [string]$Repo,
  [string]$Pattern,
  [string]$Name,
  [string]$ConfigRelative
) {
  # Exact published CHttp 2.1.0-rc.5 Owner epoch, with no fallback.
  switch ($Repo) {
    'qigao/salts' {
      $tag = 'v2.3.0-rc.15'
      $Pattern = 'Salts.Native.2.3.0-rc.15.nupkg'
    }
    'qigao/salts-utils' {
      $tag = 'v4.3.0-rc.10'
      $Pattern = 'SaltsUtils.Native.4.3.0-rc.10.nupkg'
    }
    'qigao/chttp' {
      $tag = 'v2.1.0-rc.5'
      $Pattern = 'CHttp.Native.2.1.0-rc.5.nupkg'
    }
    default {
      $releaseJson = & gh api "repos/$Repo/releases?per_page=100" --paginate --slurp
      if ($LASTEXITCODE -ne 0) { throw "failed to query releases for $Repo" }
      $releases = foreach ($page in (($releaseJson -join "`n") | ConvertFrom-Json)) { $page }
      $tag = ($releases | Where-Object { $_.draft -eq $false -and $_.published_at } |
        Sort-Object published_at | Select-Object -Last 1).tag_name
      if ([string]::IsNullOrWhiteSpace($tag)) {
        throw "failed to resolve latest published release for $Repo"
      }
    }
  }
  $tag = $tag.Trim()
  [Console]::Error.WriteLine("Restoring $Repo@$tag ($Rid)")

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
  if ($Repo -in @('qigao/salts', 'qigao/salts-utils', 'qigao/chttp')) {
    # A PowerShell function emits child stdout into its return value.
    # Keep attestation output in the log, not the SDK root assignment.
    $attestation = @(& python (Join-Path $PSScriptRoot 'verify-published-native-sdk.py') `
      --repo $Repo --archive $nupkg.FullName --extracted $package --rid $Rid 2>&1)
    if ($LASTEXITCODE -ne 0) {
      throw ('published SDK verification failed for ' + $Repo + '@' + $tag + ': ' + ($attestation -join ' '))
    }
    [Console]::Error.WriteLine(($attestation -join [Environment]::NewLine))
  }

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
