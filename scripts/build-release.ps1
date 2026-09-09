[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Python = 'python',
    [switch]$SkipRestore,
    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $repoRoot

function Invoke-ReleaseCommand {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $Command" }
}

Invoke-ReleaseCommand $Python @('scripts/release.py', 'prepare', '--version', $Version)
$artifactRoot = Join-Path $repoRoot 'output/release'
if (Test-Path -LiteralPath $artifactRoot) {
    throw 'output/release already exists. Use a clean checkout/output directory to avoid mixing releases.'
}
New-Item -ItemType Directory -Path $artifactRoot | Out-Null

if (-not $SkipRestore) {
    Invoke-ReleaseCommand npm @('ci')
    Invoke-ReleaseCommand npm @('--prefix', 'ui', 'ci')
}
Invoke-ReleaseCommand cmake @('-S', 'engine', '-B', 'engine/build', '-A', 'x64')
Invoke-ReleaseCommand cmake @('--build', 'engine/build', '--config', 'Release', '--parallel', '2')
if (-not $SkipTests) {
    Invoke-ReleaseCommand $Python @('-m', 'unittest', 'discover', '-s', 'scripts', '-p', 'release_test.py')
    Invoke-ReleaseCommand ctest @('--test-dir', 'engine/build', '-C', 'Release', '--output-on-failure')
    Invoke-ReleaseCommand npm @('run', 'test:protocol')
    Invoke-ReleaseCommand npm @('--prefix', 'ui', 'run', 'check')
    Invoke-ReleaseCommand npm @('--prefix', 'ui', 'test')
    Invoke-ReleaseCommand cargo @('test', '--workspace', '--locked')
}
Invoke-ReleaseCommand $Python @('scripts/release.py', 'notices')

# Both packages use the system Evergreen WebView2; NSIS installs it when absent.
Invoke-ReleaseCommand npm @('run', 'tauri', '--', 'build', '--ci', '--no-sign', '--bundles', 'nsis',
    '--config', 'target/release-meta/tauri.conf.json', '--', '--locked')

$portableName = "RoudaMix-$Version-windows-x64-portable"
$stage = Join-Path $repoRoot "target/release-portable/$portableName"
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -LiteralPath target/release/roudamix-app.exe -Destination (Join-Path $stage 'RoudaMix.exe')
Copy-Item -LiteralPath engine/build/Release/roudamix-engine.exe, engine/build/Release/roudamix-worker.exe -Destination $stage
Copy-Item -LiteralPath target/release-legal -Destination (Join-Path $stage 'licenses') -Recurse
Copy-Item -LiteralPath packaging/release/README.txt -Destination $stage
Set-Content -LiteralPath (Join-Path $stage 'portable.flag') -Value 'RoudaMix portable mode' -Encoding ascii

# Verify PE architecture and CRT dependencies without requiring dumpbin on PATH.
Invoke-ReleaseCommand $Python @('scripts/verify-release.py', '--stage', $stage)
Compress-Archive -LiteralPath $stage -DestinationPath (Join-Path $artifactRoot "$portableName.zip") -CompressionLevel Optimal
$installers = @(Get-ChildItem target/release/bundle/nsis -Filter '*.exe' -File)
if ($installers.Count -ne 1) { throw 'Expected exactly one NSIS installer.' }
Invoke-ReleaseCommand 7z @('x', $installers[0].FullName, '-otarget/release-installer-check', '-y')
Invoke-ReleaseCommand $Python @('scripts/verify-installer.py', '--directory', 'target/release-installer-check')
Copy-Item -LiteralPath $installers[0].FullName -Destination (Join-Path $artifactRoot "RoudaMix-$Version-windows-x64-setup.exe")
Invoke-ReleaseCommand $Python @('scripts/release.py', 'source')
Invoke-ReleaseCommand $Python @('scripts/release.py', 'notes')
Copy-Item -LiteralPath target/release-legal/THIRD-PARTY-NOTICES.txt -Destination $artifactRoot
Copy-Item -LiteralPath target/release-legal/BUILD-INFO.json -Destination $artifactRoot
$sums = Get-ChildItem -LiteralPath $artifactRoot -File | Sort-Object Name | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
}
# GNU sha256sum on the Linux publishing runner treats CR as part of the filename.
[IO.File]::WriteAllText((Join-Path $artifactRoot 'SHA256SUMS.txt'), (($sums -join "`n") + "`n"), [Text.Encoding]::ASCII)
Write-Host "Release assets ready: $artifactRoot"
