#Requires -Version 5.1
<#
.SYNOPSIS
    Builds, Bundles, and Signs AVScan.ipa for iOS/iPadOS ARM64 on Windows
.DESCRIPTION
    Executes the CMake + LLVM/Clang + TheosWin iOS ARM64 cross-compilation pipeline,
    constructs the AVScan.app bundle with Info.plist and Metal shaders, and signs
    an installable AVScan.ipa using zsign when the iOS SDK and signing credentials
    are present. Never fabricates a dummy IPA if prerequisites are missing.
#>

param(
    [string]$SdkRoot = $env:IOS_SDK_ROOT,
    [string]$CertP12 = $env:IOS_SIGN_CERT_P12,
    [string]$CertPass = $env:IOS_SIGN_CERT_PASS,
    [string]$ProvisionProfile = $env:IOS_PROVISION_PROFILE
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$AuditScript = Join-Path $PSScriptRoot "verify_toolchain.ps1"

& $AuditScript
$AuditJson = Get-Content (Join-Path $RepoRoot "build\toolchain_audit.json") | ConvertFrom-Json

if (-not $AuditJson.ios_cross_compile_ready) {
    Write-Error "BLOCKED [M0]: Cannot produce a real ARM64 iOS binary because iOS cross-compilation prerequisites are missing on this Windows host. See build/toolchain_audit.json and run scripts/install_windows_toolchain.ps1."
    exit 2
}

$IosBuildDir = Join-Path $RepoRoot "build\ios_arm64"
$AppBundleDir = Join-Path $IosBuildDir "Payload\AVScan.app"
New-Item -ItemType Directory -Path $AppBundleDir -Force | Out-Null

& $AuditJson.tools.cmake -S $RepoRoot -B $IosBuildDir `
    -G "Ninja" `
    -DCMAKE_TOOLCHAIN_FILE="$RepoRoot\cmake\ios.toolchain.cmake" `
    -DCMAKE_BUILD_TYPE=Release `
    -DAVSCAN_BUILD_IOS_APP=ON `
    -DAVSCAN_BUILD_TESTS=OFF

if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake iOS configuration failed."
    exit $LASTEXITCODE
}

& $AuditJson.tools.cmake --build $IosBuildDir --config Release
if ($LASTEXITCODE -ne 0) {
    Write-Error "CMake iOS compilation failed."
    exit $LASTEXITCODE
}

Copy-Item (Join-Path $RepoRoot "ios\AVScan\Info.plist") (Join-Path $AppBundleDir "Info.plist") -Force
Copy-Item (Join-Path $RepoRoot "ios\AVScan\Rendering\Shaders.metal") (Join-Path $AppBundleDir "Shaders.metal") -Force

if ($AuditJson.tools.codesign_tool -and $CertP12 -and $ProvisionProfile) {
    $IpaOut = Join-Path $RepoRoot "build\AVScan.ipa"
    & $AuditJson.tools.codesign_tool -k $CertP12 -p $CertPass -m $ProvisionProfile -o $IpaOut -z 9 $AppBundleDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Code signing failed."
        exit $LASTEXITCODE
    }
    Write-Host "Successfully built and signed installable IPA: $IpaOut" -ForegroundColor Green
} else {
    Write-Warning "Unsigned AVScan.app compiled at $AppBundleDir. Provide IOS_SIGN_CERT_P12 and IOS_PROVISION_PROFILE (or sign via Sideloadly) to install on iPhone/iPad."
}
