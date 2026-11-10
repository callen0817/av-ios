#Requires -Version 5.1
<#
.SYNOPSIS
    Provisions the Windows-to-iOS C++17 / Objective-C++ Cross-Compilation Toolchain
.DESCRIPTION
    Installs:
    1. Visual Studio 2022 Community C++ & LLVM/Clang workloads via vs_installer
    2. Ninja build system via winget
    3. TheosWin toolchain & iPhoneOS SDK sysroot structure under C:\theos
#>

$ErrorActionPreference = "Stop"

Write-Host "=== Step 1: Installing Visual Studio 2022 C++ & Clang Workloads ===" -ForegroundColor Cyan
$vsSetup = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\setup.exe"
if (Test-Path $vsSetup) {
    & $vsSetup modify --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" `
        --add Microsoft.VisualStudio.Workload.NativeDesktop `
        --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        --add Microsoft.VisualStudio.Component.VC.Llvm.Clang `
        --add Microsoft.VisualStudio.Component.VC.CMake.Project `
        --passive --norestart
} else {
    Write-Warning "Visual Studio Installer not found at $vsSetup"
}

Write-Host "=== Step 2: Installing LLVM & Ninja via WinGet ===" -ForegroundColor Cyan
winget install --id LLVM.LLVM -e --source winget --accept-package-agreements --accept-source-agreements
winget install --id Ninja-build.Ninja -e --source winget --accept-package-agreements --accept-source-agreements

Write-Host "=== Step 3: Initializing TheosWin Directory Structure ===" -ForegroundColor Cyan
$TheosDir = "C:\theos"
New-Item -ItemType Directory -Path "$TheosDir\sdks" -Force | Out-Null
New-Item -ItemType Directory -Path "$TheosDir\toolchain\windows\iphone\bin" -Force | Out-Null

Write-Host "Toolchain provisioning steps initiated. Place iPhoneOS16.5.sdk in C:\theos\sdks and run scripts/verify_toolchain.ps1 to confirm readiness." -ForegroundColor Green
