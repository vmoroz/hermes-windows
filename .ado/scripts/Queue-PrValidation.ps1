<#
.SYNOPSIS
  Queue a PR validation build for a branch via the Azure DevOps REST API.

.DESCRIPTION
  A dependency-bump PR cannot pass validation until the versions it introduces are
  cached in the feed. The warm pipelines cache those versions and then call this
  script to re-run the PR's validation, so warm-then-validate happens in order from
  a single trusted action.

  Authentication uses the pipeline's own System.AccessToken (the collection build
  service, scoped to this organization) - never the cross-organization feed
  identity. Grant that build service "Queue builds" on the PR validation pipeline.

  DefinitionId is left unset until the PR validation pipeline is registered; with no
  id this script reports and exits without queuing, so the warm still succeeds.

.PARAMETER Organization
  The collection URL, e.g. https://dev.azure.com/<org>/ (defaults to the running
  pipeline's collection).

.PARAMETER Project
  The team project that hosts the PR validation pipeline.

.PARAMETER DefinitionId
  The PR validation pipeline definition id. 0 (default) skips queuing.

.PARAMETER SourceBranch
  The ref to validate, e.g. refs/heads/dependabot/npm_and_yarn/... .

.PARAMETER AccessToken
  A bearer token for the REST call (defaults to System.AccessToken).
#>
[CmdletBinding()]
param(
  [string] $Organization = $env:SYSTEM_COLLECTIONURI,
  [string] $Project = $env:SYSTEM_TEAMPROJECT,
  [int] $DefinitionId = 0,
  [Parameter(Mandatory)] [string] $SourceBranch,
  [string] $AccessToken = $env:SYSTEM_ACCESSTOKEN
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if ($DefinitionId -le 0) {
  Write-Host "PR validation pipeline id is not configured; skipping re-queue for $SourceBranch."
  return
}
if ([string]::IsNullOrWhiteSpace($AccessToken)) {
  throw 'No access token available to queue the PR validation build.'
}
if ([string]::IsNullOrWhiteSpace($Organization) -or [string]::IsNullOrWhiteSpace($Project)) {
  throw 'Organization and Project are required to queue a build.'
}

$uri = "$($Organization.TrimEnd('/'))/$Project/_apis/build/builds?api-version=7.1"
$body = @{
  definition   = @{ id = $DefinitionId }
  sourceBranch = $SourceBranch
} | ConvertTo-Json

$headers = @{ Authorization = "Bearer $AccessToken" }
$resp = Invoke-RestMethod -Method Post -Uri $uri -Headers $headers -ContentType 'application/json' -Body $body
Write-Host "Queued validation build $($resp.id) for $SourceBranch -> $($resp._links.web.href)"
