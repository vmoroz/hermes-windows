<#
.SYNOPSIS
  Warm npm package versions into the ms/react-native-public Azure Artifacts feed.

.DESCRIPTION
  The CI/PR builds restore the Node-API test toolchain from the react-native-public
  feed anonymously (the feed proxies registry.npmjs.org). A version that has never
  been requested from the feed is not cached yet, so an anonymous restore gets a
  401 from the feed host until the version is pulled in. Azure Artifacts pulls a
  version from its upstream only on the first *authenticated* request, so this
  script issues that authenticated request for each target version, which makes the
  version resolvable anonymously afterwards.

  Targets are always identified by name@version and fetched through the feed, never
  by the lockfile's "resolved" URL. That keeps warming correct even when a lockfile
  still carries a registry.npmjs.org URL (for example, a freshly regenerated
  dependency-bump lockfile).

  This script only reaches the feed host; it never runs package lifecycle scripts
  (npm pack with --ignore-scripts) and never prints the token.

.PARAMETER Feed
  The npm registry URL of the feed to warm.

.PARAMETER Token
  A bearer token with rights to save packages on the feed. Optional: when omitted,
  the script acquires one from the current Azure CLI login (`az login` locally, or
  the AzureCLI task's service connection in a pipeline). Do not pass it on a shared
  shell.

.PARAMETER Packages
  Explicit name@version specs to warm (space- or comma-separated when passed from a
  pipeline parameter).

.PARAMETER LockFile
  One or more package-lock.json paths whose full closure should be warmed.

.PARAMETER Branch
  A git ref carrying a dependency bump. The lockfile at LockPathInRepo is read from
  this ref and from BaseRef, and only the added name@version set is warmed.

.PARAMETER BaseRef
  The ref to diff Branch against when computing the delta.

.PARAMETER LockPathInRepo
  Repo-relative path of the lockfile read for the Branch delta.
#>
[CmdletBinding()]
param(
  [string] $Feed = 'https://pkgs.dev.azure.com/ms/react-native/_packaging/react-native-public/npm/registry/',
  [string] $Token,
  [string[]] $Packages,
  [string[]] $LockFile,
  [string] $Branch,
  [string] $BaseRef = 'origin/main',
  [string] $LockPathInRepo = 'unittests/NodeApi/test/package-lock.json'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Turn a package-lock.json body into a sorted, de-duplicated set of name@version.
# Only registry packages (those with a version and a resolved tarball) are kept;
# workspace and linked entries have no feed tarball to warm.
function Get-SpecsFromLock {
  param([Parameter(Mandatory)] [string] $Json)

  # -AsHashtable: npm lockfiles key the root package on an empty string, which the
  # object parser rejects.
  $lock = $Json | ConvertFrom-Json -AsHashtable
  $specs = [System.Collections.Generic.SortedSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase)

  if (-not $lock.ContainsKey('packages')) {
    return $specs
  }

  foreach ($key in $lock.packages.Keys) {
    if ([string]::IsNullOrEmpty($key)) { continue }      # the root project
    $node = $lock.packages[$key]
    if ($node.ContainsKey('link') -and $node['link']) { continue }
    if (-not $node.ContainsKey('version') -or -not $node['version']) { continue }
    if (-not $node.ContainsKey('resolved') -or -not $node['resolved']) { continue }

    # The install name is the key after the last "node_modules/" segment; this is
    # correct for every dependency in this repo's toolchain.
    $marker = 'node_modules/'
    $idx = $key.LastIndexOf($marker)
    $name = if ($idx -ge 0) { $key.Substring($idx + $marker.Length) } else { $key }
    [void]$specs.Add("$name@$($node['version'])")
  }

  return $specs
}

function Read-LockFromGit {
  param([Parameter(Mandatory)] [string] $Ref, [Parameter(Mandatory)] [string] $Path)
  $body = & git show "${Ref}:${Path}" 2>$null
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($body | Out-String))) {
    throw "Could not read $Path from $Ref."
  }
  return ($body | Out-String)
}

# Issue the authenticated fetch that triggers the feed's upstream pull. npm pack
# downloads each version's tarball through the feed; --ignore-scripts guarantees no
# package code runs.
function Invoke-Warm {
  param(
    [Parameter(Mandatory)] [string[]] $Specs,
    [Parameter(Mandatory)] [string] $Feed,
    [Parameter(Mandatory)] [string] $Token
  )

  if (-not $Specs -or $Specs.Count -eq 0) {
    Write-Host 'Nothing to warm.'
    return @{ Warmed = @(); Failed = @() }
  }

  $work = Join-Path ([System.IO.Path]::GetTempPath()) ("warm-" + [Guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  $userConfig = Join-Path $work '.npmrc'
  $dest = Join-Path $work 'tgz'
  New-Item -ItemType Directory -Force -Path $dest | Out-Null

  # Per-registry bearer auth; npm sends it as "Authorization: Bearer <token>".
  $feedKey = $Feed -replace '^https:', ''
  @(
    "registry=$Feed"
    'always-auth=true'
    "${feedKey}:_authToken=$Token"
  ) | Set-Content -LiteralPath $userConfig -Encoding Ascii

  $warmed = [System.Collections.Generic.List[string]]::new()
  $failed = [System.Collections.Generic.List[string]]::new()
  try {
    $env:NPM_CONFIG_USERCONFIG = $userConfig
    $env:NPM_CONFIG_REGISTRY = $Feed

    # Pack in batches for speed; fall back to per-spec on a batch failure so one bad
    # version does not hide the rest.
    $batchSize = 40
    for ($i = 0; $i -lt $Specs.Count; $i += $batchSize) {
      $batch = $Specs[$i..([Math]::Min($i + $batchSize - 1, $Specs.Count - 1))]
      & npm pack --ignore-scripts --pack-destination $dest @batch *> $null
      if ($LASTEXITCODE -eq 0) {
        $warmed.AddRange([string[]]$batch)
        continue
      }
      foreach ($spec in $batch) {
        & npm pack --ignore-scripts --pack-destination $dest $spec *> $null
        if ($LASTEXITCODE -eq 0) { $warmed.Add($spec) } else { $failed.Add($spec) }
      }
    }
  }
  finally {
    Remove-Item Env:NPM_CONFIG_USERCONFIG -ErrorAction SilentlyContinue
    Remove-Item Env:NPM_CONFIG_REGISTRY -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
  }

  return @{ Warmed = $warmed.ToArray(); Failed = $failed.ToArray() }
}

# Resolve the feed token. Use -Token when supplied; otherwise acquire one from the
# ambient Azure CLI login, so the same script runs locally (after `az login`) and
# in a pipeline (where the AzureCLI task logs in as the service connection first).
if ([string]::IsNullOrWhiteSpace($Token)) {
  if (-not (Get-Command az -ErrorAction SilentlyContinue)) {
    throw 'No -Token was provided and the Azure CLI (az) was not found. Install it and run "az login", or pass -Token.'
  }
  # 499b84ac-... is the Azure DevOps resource id; the token authorizes feed access.
  $Token = az account get-access-token --resource 499b84ac-1321-427f-aa17-267ca6975798 --query accessToken -o tsv 2>$null
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($Token)) {
    throw 'Could not acquire a feed token from Azure CLI. Run "az login" (or pass -Token).'
  }
  # Mask the token in pipeline logs; harmless to skip when running locally.
  if ($env:TF_BUILD -eq 'True') { Write-Host "##vso[task.setsecret]$Token" }
}

# Collect the target name@version set from whichever inputs were provided.
$targets = [System.Collections.Generic.SortedSet[string]]::new(
  [System.StringComparer]::OrdinalIgnoreCase)

if ($Packages) {
  foreach ($p in ($Packages -split '[,\s]+')) {
    if (-not [string]::IsNullOrWhiteSpace($p)) { [void]$targets.Add($p.Trim()) }
  }
}

if ($LockFile) {
  foreach ($path in $LockFile) {
    $body = Get-Content -LiteralPath $path -Raw
    foreach ($s in (Get-SpecsFromLock -Json $body)) { [void]$targets.Add($s) }
  }
}

if ($Branch) {
  $branchSpecs = Get-SpecsFromLock -Json (Read-LockFromGit -Ref $Branch -Path $LockPathInRepo)
  $baseSpecs = try {
    Get-SpecsFromLock -Json (Read-LockFromGit -Ref $BaseRef -Path $LockPathInRepo)
  } catch {
    Write-Warning "Could not read the base lockfile from ${BaseRef}: warming the full branch closure."
    [System.Collections.Generic.SortedSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
  }
  foreach ($s in $branchSpecs) {
    if (-not $baseSpecs.Contains($s)) { [void]$targets.Add($s) }
  }
}

$specList = @($targets)
Write-Host "Versions to warm: $($specList.Count)"
foreach ($s in $specList) { Write-Host "  $s" }

$result = Invoke-Warm -Specs $specList -Feed $Feed -Token $Token

Write-Host "Warmed: $($result.Warmed.Count)"
if ($result.Failed.Count -gt 0) {
  Write-Host "Failed: $($result.Failed.Count)"
  foreach ($s in $result.Failed) { Write-Host "  $s" }
  throw "$($result.Failed.Count) version(s) could not be warmed (see list above)."
}
Write-Host 'Feed warm complete.'
