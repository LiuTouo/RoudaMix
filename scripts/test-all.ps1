# CI 與 release 共用的完整測試套件。
# 前置條件（呼叫端負責）：engine/build 已建置、root 與 ui 的 node_modules 已安裝。
[CmdletBinding()]
param(
    [string]$Python = 'python'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $repoRoot

function Invoke-TestCommand {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $Command" }
}

Invoke-TestCommand $Python @('-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'release_test.py')
Invoke-TestCommand ctest @('--test-dir', 'engine/build', '-C', 'Release', '--output-on-failure')
Invoke-TestCommand npm @('run', 'test:protocol')
Invoke-TestCommand npm @('--prefix', 'ui', 'run', 'check')
Invoke-TestCommand npm @('--prefix', 'ui', 'test')
Invoke-TestCommand cargo @('test', '--workspace', '--locked')
