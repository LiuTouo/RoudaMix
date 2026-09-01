[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$WebView2RuntimeDir,

    [switch]$SkipRestore,
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtimeSource = [System.IO.Path]::GetFullPath($WebView2RuntimeDir)
$runtimeCache = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'src-tauri\WebView2Runtime'))
$workRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'target\portable-package'))
$outputRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'output\portable'))
Set-Location -LiteralPath $repoRoot

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')"
    }
}

function Assert-WorkspaceChild {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    $prefix = $repoRoot.TrimEnd('\') + '\'
    if (-not $full.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to modify a path outside the repository: $full"
    }
}

function Find-Dumpbin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'dumpbin.exe not found and vswhere.exe is unavailable.'
    }
    $installation = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $installation) {
        throw 'Visual Studio C++ x64 build tools are not installed.'
    }
    $candidate = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') `
        -Filter dumpbin.exe -File -Recurse |
        Where-Object { $_.FullName -match '\\bin\\Hostx64\\x64\\dumpbin\.exe$' } |
        Sort-Object FullName -Descending |
        Select-Object -First 1
    if (-not $candidate) {
        throw 'Could not locate the x64 dumpbin.exe in Visual Studio Build Tools.'
    }
    return $candidate.FullName
}

function Assert-PortableBinary {
    param(
        [Parameter(Mandatory = $true)][string]$Dumpbin,
        [Parameter(Mandatory = $true)][string]$Binary
    )
    Assert-X64Binary -Dumpbin $Dumpbin -Binary $Binary
    $dependents = & $Dumpbin /dependents $Binary 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin /dependents failed: $Binary"
    }
    $dependencyText = $dependents -join "`n"
    if ($dependencyText -match '(?im)\b(?:MSVCP|VCRUNTIME)\d+(?:_\d+)?\.dll\b') {
        throw "Binary still depends on the Visual C++ Redistributable: $Binary"
    }
}

function Assert-X64Binary {
    param(
        [Parameter(Mandatory = $true)][string]$Dumpbin,
        [Parameter(Mandatory = $true)][string]$Binary
    )
    $headers = & $Dumpbin /headers $Binary 2>&1
    if ($LASTEXITCODE -ne 0 -or ($headers -join "`n") -notmatch '8664 machine \(x64\)') {
        throw "Binary is not Windows x64: $Binary"
    }
}

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    $stream = [System.IO.File]::OpenRead($Path)
    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha256.ComputeHash($stream)
        return [System.BitConverter]::ToString($bytes).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $sha256.Dispose()
        $stream.Dispose()
    }
}

if (-not (Test-Path -LiteralPath $runtimeSource -PathType Container)) {
    throw "WebView2 runtime directory does not exist: $runtimeSource"
}
if (-not (Test-Path -LiteralPath (Join-Path $runtimeSource 'msedgewebview2.exe') -PathType Leaf)) {
    throw 'Pass the extracted x64 Fixed Version Runtime folder that directly contains msedgewebview2.exe.'
}
$evergreenRoot = [System.IO.Path]::GetFullPath((Join-Path ${env:ProgramFiles(x86)} 'Microsoft\EdgeWebView\Application'))
if ($runtimeSource.Equals($evergreenRoot, [System.StringComparison]::OrdinalIgnoreCase) -or
    $runtimeSource.StartsWith($evergreenRoot.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'The installed Evergreen WebView2 directory is not a redistributable Fixed Version Runtime.'
}
if ($runtimeSource.Equals($runtimeCache, [System.StringComparison]::OrdinalIgnoreCase) -or
    $runtimeSource.StartsWith($runtimeCache.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'WebView2RuntimeDir cannot be inside src-tauri\WebView2Runtime.'
}

Assert-WorkspaceChild $runtimeCache
Assert-WorkspaceChild $workRoot
Assert-WorkspaceChild $outputRoot

if (-not $SkipRestore) {
    Invoke-Native npm @('ci')
    Invoke-Native npm @('--prefix', 'ui', 'ci')
}

Invoke-Native cmake @('-S', 'engine', '-B', 'engine/build')
Invoke-Native cmake @('--build', 'engine/build', '--config', 'Release')

if (-not $SkipTests) {
    Invoke-Native ctest @('--test-dir', 'engine/build', '-C', 'Release', '--output-on-failure')
    Invoke-Native cargo @('test', '--workspace')
    Invoke-Native npm @('--prefix', 'ui', 'run', 'check')
    Invoke-Native npm @('--prefix', 'ui', 'test')
}

if (Test-Path -LiteralPath $runtimeCache) {
    Remove-Item -LiteralPath $runtimeCache -Recurse -Force
}
New-Item -ItemType Directory -Path $runtimeCache | Out-Null
Get-ChildItem -LiteralPath $runtimeSource -Force |
    Copy-Item -Destination $runtimeCache -Recurse -Force

Invoke-Native npm @(
    'run', 'tauri', '--', 'build', '--no-bundle', '--no-sign', '--ci',
    '--config', 'src-tauri/tauri.portable.conf.json'
)

$tauriConfig = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'src-tauri\tauri.conf.json') |
    ConvertFrom-Json
$version = [string]$tauriConfig.version
$artifactName = "RoudaMix-$version-windows-x64-portable"
$stageRoot = Join-Path $workRoot $artifactName
$zipPath = Join-Path $workRoot "$artifactName.zip"
$zipHashPath = "$zipPath.sha256"

Assert-WorkspaceChild $stageRoot
Assert-WorkspaceChild $zipPath
if (Test-Path -LiteralPath $workRoot) {
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stageRoot | Out-Null

$releaseRoot = Join-Path $repoRoot 'target\release'
$mainExe = Join-Path $releaseRoot 'roudamix-app.exe'
$engineExe = Join-Path $releaseRoot 'roudamix-engine.exe'
$workerExe = Join-Path $releaseRoot 'roudamix-worker.exe'
$builtRuntime = $runtimeCache
foreach ($required in @($mainExe, $engineExe, $workerExe)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Expected build output is missing: $required"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $builtRuntime 'msedgewebview2.exe') -PathType Leaf)) {
    throw "Fixed WebView2 runtime staging cache is incomplete: $builtRuntime"
}

$dumpbin = Find-Dumpbin
Assert-X64Binary -Dumpbin $dumpbin -Binary (Join-Path $builtRuntime 'msedgewebview2.exe')
foreach ($binary in @($mainExe, $engineExe, $workerExe)) {
    Assert-PortableBinary -Dumpbin $dumpbin -Binary $binary
}

Copy-Item -LiteralPath $mainExe -Destination (Join-Path $stageRoot 'RoudaMix.exe')
Copy-Item -LiteralPath $engineExe -Destination $stageRoot
Copy-Item -LiteralPath $workerExe -Destination $stageRoot
Copy-Item -LiteralPath $builtRuntime -Destination $stageRoot -Recurse
Set-Content -LiteralPath (Join-Path $stageRoot 'portable.flag') `
    -Value 'RoudaMix portable mode' -Encoding ASCII
$runtimeVersion = (Get-Item -LiteralPath (Join-Path $builtRuntime 'msedgewebview2.exe')).VersionInfo.ProductVersion
Set-Content -LiteralPath (Join-Path $stageRoot 'WEBVIEW2-RUNTIME.txt') `
    -Value @('Microsoft Edge WebView2 Fixed Version Runtime', 'Architecture: x64', "Version: $runtimeVersion") `
    -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\portable\README.txt') -Destination $stageRoot
Copy-Item -LiteralPath (Join-Path $repoRoot 'packaging\portable\THIRD-PARTY-NOTICES.txt') -Destination $stageRoot

$licenses = Join-Path $stageRoot 'LICENSES'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $repoRoot 'engine\third_party\vst3-sdk\LICENSE.txt') `
    -Destination (Join-Path $licenses 'VST3-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $repoRoot 'engine\third_party\asio-sdk\LICENSE.txt') `
    -Destination (Join-Path $licenses 'ASIO-LICENSE.txt')

$manifestPath = Join-Path $stageRoot 'SHA256SUMS.txt'
$manifestLines = Get-ChildItem -LiteralPath $stageRoot -File -Recurse |
    Where-Object { $_.FullName -ne $manifestPath } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = $_.FullName.Substring($stageRoot.Length + 1).Replace('\', '/')
        $hash = Get-Sha256 -Path $_.FullName
        "$hash  $relative"
    }
Set-Content -LiteralPath $manifestPath -Value $manifestLines -Encoding UTF8

Compress-Archive -LiteralPath $stageRoot -DestinationPath $zipPath -CompressionLevel Optimal
$zipHash = Get-Sha256 -Path $zipPath
Set-Content -LiteralPath $zipHashPath -Value "$zipHash  $([System.IO.Path]::GetFileName($zipPath))" `
    -Encoding ASCII

if (Test-Path -LiteralPath $outputRoot) {
    Remove-Item -LiteralPath $outputRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $outputRoot | Out-Null
Copy-Item -LiteralPath $stageRoot -Destination $outputRoot -Recurse
Copy-Item -LiteralPath $zipPath -Destination $outputRoot
Copy-Item -LiteralPath $zipHashPath -Destination $outputRoot

$outputZip = Join-Path $outputRoot ([System.IO.Path]::GetFileName($zipPath))
Remove-Item -LiteralPath $workRoot -Recurse -Force
Remove-Item -LiteralPath $runtimeCache -Recurse -Force
Write-Host "Portable package: $outputZip"
Write-Host "SHA-256: $zipHash"
