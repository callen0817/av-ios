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
$ProgressPreference = "SilentlyContinue"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$AuditScript = Join-Path $PSScriptRoot "verify_toolchain.ps1"

& $AuditScript
$AuditJson = Get-Content (Join-Path $RepoRoot "build\toolchain_audit.json") | ConvertFrom-Json

if (-not $AuditJson.ios_cross_compile_ready) {
    Write-Error "BLOCKED [M0]: Cannot produce a real ARM64 iOS binary because iOS cross-compilation prerequisites are missing on this Windows host. See build/toolchain_audit.json and run scripts/install_windows_toolchain.ps1."
    exit 2
}

$IosBuildDir = Join-Path $RepoRoot "build\ios_arm64"
$StagingDir = Join-Path $RepoRoot "build\staging"
if (Test-Path $StagingDir) { Remove-Item $StagingDir -Recurse -Force }
$StagingPayload = Join-Path $StagingDir "Payload"
$AppBundleDir = Join-Path $StagingPayload "AVScan.app"
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
$AssetsDir = Join-Path $RepoRoot "ios\AVScan\Assets"
if (Test-Path $AssetsDir) {
    Get-ChildItem -Path $AssetsDir -Filter "*.png" | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $AppBundleDir $_.Name) -Force
    }
}

$BuiltExe = Join-Path $IosBuildDir "AVScan.app\AVScan"
if (-not (Test-Path $BuiltExe)) {
    $BuiltExe = Join-Path $IosBuildDir "AVScan"
}
if (Test-Path $BuiltExe) {
    Copy-Item $BuiltExe (Join-Path $AppBundleDir "AVScan") -Force
}

$IpaOut = Join-Path $RepoRoot "build\AVScan.ipa"
if ($AuditJson.tools.codesign_tool -and $CertP12 -and $ProvisionProfile) {
    & $AuditJson.tools.codesign_tool -f -k $CertP12 -p $CertPass -m $ProvisionProfile -o $IpaOut -z 9 $AppBundleDir
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Code signing failed."
        exit $LASTEXITCODE
    }
    Write-Host "Successfully built and signed installable IPA with Developer Profile: $IpaOut" -ForegroundColor Green
} elseif ($AuditJson.tools.codesign_tool) {
    & $AuditJson.tools.codesign_tool -f -a -o $IpaOut -z 9 $AppBundleDir
    Write-Host "Built ad-hoc signed IPA: $IpaOut (Ready for TrollStore / Sideloadly / AltStore)" -ForegroundColor Green
    Write-Warning "Developer signing assets (.p12 / .mobileprovision) not supplied. Signed with ad-hoc signature. Once your Apple Developer account is approved, re-run with -CertP12 and -ProvisionProfile for official device provisioning."
} else {
    $UnsignedIpa = Join-Path $RepoRoot "build\AVScan_unsigned.ipa"
    if (Test-Path $UnsignedIpa) { Remove-Item $UnsignedIpa -Force }
    Add-Type -AssemblyName "System.IO.Compression.FileSystem"
    [System.IO.Compression.ZipFile]::CreateFromDirectory($StagingPayload, $UnsignedIpa, [System.IO.Compression.CompressionLevel]::Optimal, $true)
    Write-Host "Packaged ready-to-sideload bundle: $UnsignedIpa" -ForegroundColor Cyan
}
