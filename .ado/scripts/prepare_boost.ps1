[cmdletbinding(SupportsShouldProcess=$True)]
param(
    [Parameter(Mandatory=$False)]
    [string]$BOOST_VERSION="1.72.0",

    [Parameter(Mandatory=$False)]
    [switch]$Force
)

$BOOST_DIR="$PWD\boost"

$BOOST_ASIO="$BOOST_DIR\lib\native\include\boost\asio.hpp"
if ((Test-Path -Path $BOOST_ASIO) -and !$Force.IsPresent) {
    Write-Output "Boost is already prepared." | Out-Null
    return $BOOST_DIR;
}

Write-Output "Preparing boost" | Out-Null

$NugetURL = 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe'
Invoke-WebRequest -Uri $NugetURL -OutFile "nuget.exe" | Out-Null
Invoke-Expression ".\nuget.exe sources Enable -Name nuget.org" | Out-Null

$NugetCommand=".\nuget.exe install boost -Version $BOOST_VERSION -Config $PSScriptRoot\Nuget.Config -OutputDirectory $PWD -NonInteractive"
Write-Output $NugetCommand | Out-Null
Invoke-Expression $NugetCommand | Out-Null

if (Test-Path -Path $BOOST_DIR){
    Remove-Item -Recurse -Force $BOOST_DIR | Out-Null
}
Rename-Item ".\boost.$BOOST_VERSION.0" $BOOST_DIR | Out-Null
return $BOOST_DIR