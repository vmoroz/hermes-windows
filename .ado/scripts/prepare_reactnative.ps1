[cmdletbinding(SupportsShouldProcess=$True)]
param(
    [Parameter(Mandatory=$False)]
    [string]$RN_CLONE_URL = "https://github.com/facebook/react-native.git",

    [Parameter(Mandatory=$False)]
    [string]$COMMIT_HASH="386dbd943",

    [Parameter(Mandatory=$False)]
    [switch]$Force
)

$RN_REPO_NAME = "react-native"
$RN_DIR="$PWD\$RN_REPO_NAME"

if($Force.IsPresent) {
    if (Test-Path -Path $RN_REPO_NAME){
        Remove-Item -Recurse -Force $RN_REPO_NAME  | Out-Null
    }
}

# We have an existing checkout if git head exists.
$RN_REPO_HEAD="$RN_DIR\.git\HEAD"
if (!(Test-Path -Path $RN_REPO_HEAD)) {
    if (Test-Path -Path $RN_REPO_NAME) {
        Remove-Item -Recurse -Force $RN_REPO_NAME  | Out-Null
    }

    Invoke-Expression "git clone $RN_CLONE_URL"  | Out-Null
} 

Push-Location $RN_REPO_NAME
Invoke-Expression "git checkout $COMMIT_HASH"  | Out-Null
Pop-Location

# extern IInspector &getInspectorInstance();
# extern __declspec(dllexport) IInspector &getInspectorInstance();
$RN_INSPECTORINTERFACES_H="$RN_DIR/ReactCommon/jsinspector/InspectorInterfaces.h"
(Get-Content -path $RN_INSPECTORINTERFACES_H ) -Replace "extern IInspector &getInspectorInstance", "extern __declspec(dllexport) IInspector& __cdecl getInspectorInstance" | Set-Content -Path $RN_INSPECTORINTERFACES_H | Out-Null

# extern void enableDebugging(
# extern __declspec(dllexport) void enableDebugging(
$RN_INSPECTOR_REGISTRATION_H="$RN_DIR/ReactCommon/hermes/inspector/chrome/Registration.h"
(Get-Content -path $RN_INSPECTOR_REGISTRATION_H) -Replace "extern void enableDebugging", "extern __declspec(dllexport) void __cdecl enableDebugging" | Set-Content -Path $RN_INSPECTOR_REGISTRATION_H | Out-Null
(Get-Content -path $RN_INSPECTOR_REGISTRATION_H) -Replace "extern void disableDebugging", "extern __declspec(dllexport) void __cdecl disableDebugging" | Set-Content -Path $RN_INSPECTOR_REGISTRATION_H | Out-Null

return $RN_DIR