[cmdletbinding(SupportsShouldProcess=$True)]
param(
    [Parameter(Mandatory=$False)]
    [string]$FOLLY_VERSION="2020.01.13.00",

    [Parameter(Mandatory=$False)]
    [string]$DOWNLOADDIR="downloads",

    [Parameter(Mandatory=$False)]
    [switch]$Force
)

$FOLLY_DIR="$PWD\folly"

$FOLLY_DYNAMIC="$FOLLY_DIR/folly/dynamic.cpp"
if ((Test-Path -Path $FOLLY_DYNAMIC) -and !$Force.IsPresent) {
    Write-Output "Folly is already prepared." | Out-Null
    return $FOLLY_DIR;
} 

Write-Output "Preparing Folly" | Out-Null

$FOLLY_URL="https://github.com/facebook/folly/archive/v${FOLLY_VERSION}.tar.gz"

if (!(Test-Path -Path $DOWNLOADDIR)) {
    New-Item -ItemType "directory" -Path $DOWNLOADDIR | Out-Null
}

$FOLLY_DOWNLOADED="$DOWNLOADDIR\v${FOLLY_VERSION}.tar.gz"
Invoke-WebRequest -Uri $FOLLY_URL -OutFile $FOLLY_DOWNLOADED | Out-Null

Invoke-Expression "$PSScriptRoot\Extract-TarGz.ps1 -FileToExtract $FOLLY_DOWNLOADED -TargetFolder $PWD" | Out-Null

if (Test-Path -Path $FOLLY_DIR){
    Remove-Item -Recurse -Force $FOLLY_DIR | Out-Null
}
Rename-Item "$PWD\folly-$FOLLY_VERSION" $FOLLY_DIR | Out-Null

# #if !defined(_MSC_VER) || (_MSC_VER < 1923)
# #if !defined(_MSC_VER) || _MSC_VER < 1923 || _MSC_VER >= 1928
$FOLLY_BUILTINS_H="$FOLLY_DIR/folly/portability/Builtins.h"
Write-Output $FOLLY_BUILTINS_H | Out-Null
(Get-Content -path $FOLLY_BUILTINS_H) -replace("#if !defined\(_MSC_VER\) \|\| \(_MSC_VER < 1923\)", "#if !defined(_MSC_VER) || _MSC_VER < 1923 || _MSC_VER >= 1928") | Set-Content -Path $FOLLY_BUILTINS_H | Out-Null

return $FOLLY_DIR