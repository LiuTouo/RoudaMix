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
    Invoke-ReleaseCommand powershell @('-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', 'scripts/test-all.ps1', '-Python', $Python)
}
Invoke-ReleaseCommand $Python @('scripts/release.py', 'notices')

# Both packages use the system Evergreen WebView2; NSIS installs it when absent.
Invoke-ReleaseCommand npm @('run', 'tauri', '--', 'build', '--ci', '--bundles', 'nsis',
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

# In-app updater: signed manifest + signature published alongside the installer.
# Tripwire: verify the artifact against the pubkey committed in tauri.conf.json
# BEFORE publish, so key/pubkey mismatch fails the build, not every user's app.
$setupArtifact = Join-Path $artifactRoot "RoudaMix-$Version-windows-x64-setup.exe"
$sigBundle = "$($installers[0].FullName).sig"
if (-not (Test-Path -LiteralPath $sigBundle)) { throw 'Updater signature (.sig) was not produced; is TAURI_SIGNING_PRIVATE_KEY set?' }
$sigArtifact = Join-Path $artifactRoot "RoudaMix-$Version-windows-x64-setup.exe.sig"
Copy-Item -LiteralPath $sigBundle -Destination $sigArtifact
$pubkey = (Get-Content src-tauri/tauri.conf.json -Raw | ConvertFrom-Json).plugins.updater.pubkey
# tauri CLI 沒有 signer verify 子命令;用 node 內建 crypto 複刻 minisign-verify 驗章。
Invoke-ReleaseCommand node @('scripts/verify-updater-sig.mjs', $setupArtifact, $sigArtifact, $pubkey)
Invoke-ReleaseCommand $Python @('scripts/release.py', 'latest-json', '--sig', $sigArtifact)

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
