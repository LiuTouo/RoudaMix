# Execute the real release script with mocked external tools; never build or publish.
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot 'build-release.ps1'
$root = Split-Path $PSScriptRoot
$release = Get-Content "$root/.github/workflows/release.yml" -Raw
$ci = Get-Content "$root/.github/workflows/ci.yml" -Raw
$buildScript = Get-Content $source -Raw
# Tripwires for the split-job release contract (workflow syntax is checked by actionlint).
foreach ($required in @('needs: [build, test]', 'run: ./scripts/test-all.ps1',
        'shared-key: windows-release-v1', 'shared-key: windows-test-v1',
        'sha256sum --check SHA256SUMS.txt', 'gh release create', '--draft')) {
    if (-not $release.Contains($required)) { throw "Missing release gate: $required" }
}
foreach ($required in @('shared-key: windows-release-v1', 'shared-key: windows-test-v1',
        "if: github.ref == 'refs/heads/main' && github.event_name != 'pull_request'",
        "if: steps.release-cache.outputs.cache-hit != 'true'",
        'npm run tauri -- build --ci --no-bundle -- --locked')) {
    if (-not $ci.Contains($required)) { throw "Missing cache contract: $required" }
}
foreach ($required in @('scripts/verify-release.py', 'scripts/verify-installer.py',
        'scripts/verify-updater-sig.mjs', "'latest-json'", "'source'", 'SHA256SUMS.txt')) {
    if (-not $buildScript.Contains($required)) { throw "Missing artifact check: $required" }
}
if ($release.Contains('continue-on-error: true')) { throw 'Release failures must block publication' }
$originalLocation = Get-Location
$temp = Join-Path ([IO.Path]::GetTempPath()) ([guid]::NewGuid().ToString())
New-Item -ItemType Directory -Path "$temp/scripts" | Out-Null
Copy-Item $source "$temp/scripts/build-release.ps1"
$originalSummary = $env:GITHUB_STEP_SUMMARY
$env:GITHUB_STEP_SUMMARY = "$temp/summary.md"
try {
    foreach ($skipTests in @($false, $true)) {
        foreach ($failBuild in @($false, $true)) {
            $calls = [Collections.Generic.List[string]]::new()
            function python {
                $global:LASTEXITCODE = 0
                if ($args[1] -eq 'notices') { throw 'TEST_STOP_BEFORE_PACKAGING' }
            }
            function npm { $global:LASTEXITCODE = 0 }
            function cmake {
                $calls.Add(($args -join ' '))
                $global:LASTEXITCODE = if ($failBuild -and $args[0] -eq '--build') { 17 } else { 0 }
            }
            function powershell {
                $calls.Add('full-test-suite')
                $global:LASTEXITCODE = 0
            }
            $caught = ''
            try {
                & "$temp/scripts/build-release.ps1" -Version '1.2.3' -SkipTests:$skipTests
            } catch {
                $caught = $_.Exception.Message
            }
            $expected = if ($failBuild) { 'Command failed (17): cmake' } else { 'TEST_STOP_BEFORE_PACKAGING' }
            if ($caught -ne $expected) { throw "Unexpected stop: $caught (expected $expected)" }
            $build = @($calls | Where-Object { $_ -like '--build *' })
            if ($build.Count -ne 1) { throw 'Expected exactly one engine build' }
            $targeted = $build[0] -match '--target roudamix-engine roudamix-worker'
            if ($targeted -ne $skipTests) { throw 'Packaging must build only production targets; local full releases must build all tests' }
            $tested = $calls.Contains('full-test-suite')
            if ($tested -ne (-not $skipTests -and -not $failBuild)) { throw 'Full test suite gate changed' }
            Remove-Item "$temp/output" -Recurse -Force
        }
    }
    if (-not (Get-Content $env:GITHUB_STEP_SUMMARY -Raw).Contains('cmake --build:')) {
        throw 'Build duration missing from job summary'
    }
    Write-Host 'Release workflow contracts and command regression checks passed (4 scenarios).'
} finally {
    $env:GITHUB_STEP_SUMMARY = $originalSummary
    Set-Location $originalLocation
    Remove-Item $temp -Recurse -Force
}
