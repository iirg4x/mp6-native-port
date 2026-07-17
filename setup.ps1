#Requires -Version 5.1
# mp6-native local setup tool -- PowerShell launcher. See setup/README.md.
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$py = Get-Command py -ErrorAction SilentlyContinue
if ($py) {
    & py -3 (Join-Path $ScriptDir "setup\setup.py") @args
    exit $LASTEXITCODE
}

$py = Get-Command python -ErrorAction SilentlyContinue
if ($py) {
    & python (Join-Path $ScriptDir "setup\setup.py") @args
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "Python 3 was not found on PATH."
Write-Host "Install it from https://www.python.org/downloads/ (or the Microsoft"
Write-Host 'Store "Python 3" package), then re-run this script.'
Write-Host ""
exit 1
