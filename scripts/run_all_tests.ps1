#Requires -Version 5.1
$ErrorActionPreference = "Stop"

$RootDir = Split-Path -Parent $PSScriptRoot
$PythonExe = "C:\Users\calle\AppData\Local\Programs\Python\Python312\python.exe"
if (-not (Test-Path $PythonExe)) {
    $PythonCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($PythonCmd) { $PythonExe = $PythonCmd.Source }
}

Write-Host "=== Running Toolchain Verification (M0) ===" -ForegroundColor Cyan
& "$PSScriptRoot\verify_toolchain.ps1"

Write-Host "`n=== Running Automated Verification & Interoperability Suite (M0-M11) ===" -ForegroundColor Cyan
& $PythonExe "$RootDir\tests\run_verification_suite.py"
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
