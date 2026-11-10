#Requires -Version 5.1
<#
.SYNOPSIS
    M0 Automated Windows-to-iOS Toolchain & Host Environment Diagnostic Audit
.DESCRIPTION
    Audits the local Windows host for:
    1. Host C++ build tools (CMake, Visual Studio 2022, MSVC cl.exe, LLVM/Clang)
    2. Windows-to-iOS ARM64 cross-compilation toolchain (TheosWin, clang++ arm64-apple-ios, ld64.lld)
    3. Apple iPhoneOS SDK sysroot (iPhoneOS.sdk with UIKit, Metal, ARKit, AVFoundation headers/tbd)
    4. iOS code-signing & IPA installation utilities (zsign, ldid, ideviceinstaller, Sideloadly)
    5. Python 3.12 verification environment (numpy, scipy, open3d, pye57, plyfile)
    Writes a structured report to build/toolchain_audit.json and prints the exact status.
#>

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build"
if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
}

function Find-BinaryPath {
    param([string[]]$Names, [string[]]$ExtraDirs = @())
    foreach ($name in $Names) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
        foreach ($dir in $ExtraDirs) {
            if ($dir -and (Test-Path (Join-Path $dir $name))) {
                return (Join-Path $dir $name)
            }
        }
    }
    return $null
}

$vswherePath = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstallPath = $null
$vsVersion = $null
if (Test-Path $vswherePath) {
    $vsJson = & $vswherePath -all -format json | ConvertFrom-Json
    if ($vsJson -and $vsJson.Count -gt 0) {
        $vsInstallPath = $vsJson[0].installationPath
        $vsVersion = $vsJson[0].installationVersion
    }
}

$msvcCl = $null
if ($vsInstallPath -and (Test-Path (Join-Path $vsInstallPath "VC\Tools\MSVC"))) {
    $clSearch = Get-ChildItem -Path (Join-Path $vsInstallPath "VC\Tools\MSVC") -Recurse -Filter "cl.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($clSearch) { $msvcCl = $clSearch.FullName }
}

$theosRoot = $env:THEOS
if (-not $theosRoot -and (Test-Path "C:\theos")) { $theosRoot = "C:\theos" }
if (-not $theosRoot -and (Test-Path "$HOME\theos")) { $theosRoot = "$HOME\theos" }

$clangPath = Find-BinaryPath -Names @("clang++.exe", "clang++") -ExtraDirs @(
    "C:\Program Files\LLVM\bin",
    $(if ($vsInstallPath) { Join-Path $vsInstallPath "VC\Tools\Llvm\x64\bin" } else { $null }),
    $(if ($theosRoot) { Join-Path $theosRoot "toolchain\windows\iphone\bin" } else { $null })
)

$ld64Path = Find-BinaryPath -Names @("ld64.lld.exe", "ld64.lld", "lld.exe") -ExtraDirs @(
    "C:\Program Files\LLVM\bin",
    $(if ($theosRoot) { Join-Path $theosRoot "toolchain\windows\iphone\bin" } else { $null })
)

$iosSdkRoot = $env:IOS_SDK_ROOT
if (-not $iosSdkRoot -and $theosRoot -and (Test-Path (Join-Path $theosRoot "sdks"))) {
    $sdkDirs = Get-ChildItem -Path (Join-Path $theosRoot "sdks") -Directory -Filter "iPhoneOS*.sdk" -ErrorAction SilentlyContinue
    if ($sdkDirs) { $iosSdkRoot = $sdkDirs[-1].FullName }
}

$zsignPath = Find-BinaryPath -Names @("zsign.exe", "zsign", "ldid.exe", "ldid") -ExtraDirs @(
    (Join-Path $RepoRoot "bin"),
    (Join-Path $RepoRoot "zsign_src\bin"),
    (Join-Path $RepoRoot "zsign_src\build\windows\vs2022\x64\Release"),
    "C:\tools\zsign",
    $(if ($theosRoot) { Join-Path $theosRoot "bin" } else { $null })
)

$installerPath = Find-BinaryPath -Names @("ideviceinstaller.exe", "Sideloadly.exe") -ExtraDirs @(
    "$env:LocalAppData\Sideloadly",
    "C:\Program Files\libimobiledevice"
)

$cmakePath = Find-BinaryPath -Names @("cmake.exe") -ExtraDirs @("C:\Program Files\CMake\bin")
$gitPath = Find-BinaryPath -Names @("git.exe") -ExtraDirs @("C:\Program Files\Git\cmd")
$dotnetPath = Find-BinaryPath -Names @("dotnet.exe") -ExtraDirs @("C:\Program Files\dotnet")
$py312Path = "C:\Users\calle\AppData\Local\Programs\Python\Python312\python.exe"
if (-not (Test-Path $py312Path)) {
    $py312Path = Find-BinaryPath -Names @("python.exe")
}

$blockers = @()
if (-not $msvcCl -and -not $clangPath) {
    $blockers += "Host C++ compiler (MSVC cl.exe or LLVM clang++.exe) is not installed in Visual Studio 2022 Community (C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools is absent)."
}
if (-not $clangPath -or -not $ld64Path) {
    $blockers += "LLVM/Clang cross-compiler with Apple Mach-O linker (ld64.lld) is not installed for arm64-apple-ios16.0 target compilation."
}
if (-not $iosSdkRoot -or -not (Test-Path $iosSdkRoot)) {
    $blockers += "Apple iPhoneOS.sdk sysroot (headers & .tbd stubs for UIKit, Metal, ARKit, AVFoundation) is not present in IOS_SDK_ROOT or THEOS/sdks."
}
if (-not $zsignPath) {
    $blockers += "iOS code-signing utility (zsign / ldid) and Apple Developer provisioning profile (.mobileprovision + .p12 certificate) are not configured on host."
}

$audit = [ordered]@{
    timestamp_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    host_os = [System.Environment]::OSVersion.VersionString
    tools = [ordered]@{
        cmake = $cmakePath
        git = $gitPath
        dotnet = $dotnetPath
        python = $py312Path
        vs2022_install_path = $vsInstallPath
        vs2022_version = $vsVersion
        host_cxx_compiler = $(if ($msvcCl) { $msvcCl } else { $clangPath })
        ios_clangxx = $clangPath
        ios_macho_linker = $ld64Path
        theos_root = $theosRoot
        ios_sdk_sysroot = $iosSdkRoot
        codesign_tool = $zsignPath
        device_installer = $installerPath
    }
    host_cxx_ready = [bool]($msvcCl -or $clangPath)
    ios_cross_compile_ready = [bool]($clangPath -and $ld64Path -and $iosSdkRoot -and (Test-Path $iosSdkRoot))
    ios_signing_ready = [bool]($zsignPath -and $env:IOS_SIGN_CERT_P12 -and $env:IOS_PROVISION_PROFILE)
    unresolved_platform_blockers = $blockers
}

$auditFile = Join-Path $BuildDir "toolchain_audit.json"
$audit | ConvertTo-Json -Depth 5 | Set-Content -Path $auditFile -Encoding UTF8

Write-Host "=== AV Scan iOS -- M0 Windows Toolchain Audit ===" -ForegroundColor Cyan
Write-Host "CMake:            $($audit.tools.cmake)"
Write-Host "Git:              $($audit.tools.git)"
Write-Host "Python:           $($audit.tools.python)"
Write-Host "VS 2022 Path:     $($audit.tools.vs2022_install_path) (v$($audit.tools.vs2022_version))"
Write-Host "Host C++:         $(if ($audit.host_cxx_ready) { $audit.tools.host_cxx_compiler } else { 'MISSING' })"
Write-Host "iOS Clang++:      $(if ($audit.tools.ios_clangxx) { $audit.tools.ios_clangxx } else { 'MISSING' })"
Write-Host "iOS SDK Sysroot:  $(if ($audit.tools.ios_sdk_sysroot) { $audit.tools.ios_sdk_sysroot } else { 'MISSING' })"
Write-Host "iOS Code Signer:  $(if ($audit.tools.codesign_tool) { $audit.tools.codesign_tool } else { 'MISSING' })"
Write-Host "Audit Saved To:   $auditFile"

if ($blockers.Count -gt 0) {
    Write-Host "`nDocumented Windows-to-iOS Build Blockers ($($blockers.Count)):" -ForegroundColor Yellow
    foreach ($b in $blockers) {
        Write-Host "  - $b" -ForegroundColor Yellow
    }
}
