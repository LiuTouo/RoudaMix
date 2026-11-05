# 暫時性本地發布腳本：帳號 Actions 額度鎖定期間，以本機取代 release.yml 的 build+publish。
# 步驟與 CI 完全對齊：版本/衝突驗證 -> 建置+完整測試+打包 -> SHA256/資產檢查 -> 建 tag -> 推送 -> GitHub Release。
# 恢復 CI 發布：把 release.yml 的 push: tags: ['v*'] 觸發加回即可。
# 前置：gh 已登入、rust 1.97.1、7z、ffmpeg、~/.tauri/roudamix.key（加密金鑰，每次執行輸入 -KeyPassword）。
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Version,
    # 簽章金鑰密碼（每次執行都必須提供；省略參數時會以遮罩提示輸入，不落地環境變數或檔案）。
    [Parameter(Mandatory = $true)][securestring]$KeyPassword,
    # 保留為 GitHub 草稿 release，先檢查下載檔案再手動公開。
    [switch]$Draft,
    # 緊急時略過完整測試（不建議；正式發布應保留測試門檻）。
    [switch]$SkipTests,
    # 只跑前置檢查與版本／衝突驗證，不建置、不發布。
    [switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $repoRoot

function Invoke-ReleaseCommand {
    param([string]$Command, [string[]]$Arguments)
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        & $Command @Arguments
        if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $Command" }
    } finally {
        Write-Host ("[release timing] {0} {1}: {2:N1}s" -f $Command, $Arguments[0], $timer.Elapsed.TotalSeconds)
    }
}

function Assert-Tool {
    param([string]$Name)
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing required tool: $Name"
    }
}

# 探測指令（預期可能失敗）：暫時降回 Continue，stderr 不會在 EAP=Stop 下被提升為終止錯誤。
function Test-CommandResult {
    param([scriptblock]$Probe)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Probe
        return ($LASTEXITCODE -eq 0)
    } finally {
        $ErrorActionPreference = $previous
    }
}

# --- 前置檢查 ---
foreach ($tool in @('git', 'gh', 'node', 'npm', 'cmake', 'python', 'cargo', '7z')) { Assert-Tool $tool }
if (-not $SkipTests) { Assert-Tool 'ffmpeg' }

# 只要求 tracked 檔案乾淨；untracked 的本機工具目錄不影響 release.py 的來源打包（僅收 tracked 檔）。
$dirty = git status --porcelain --untracked-files=no
if ($dirty) { throw "Working tree has uncommitted tracked changes; commit or stash first:`n$dirty" }

if (-not (Test-CommandResult { gh auth status > $null 2>&1 })) {
    throw 'gh CLI is not authenticated; run: gh auth login'
}

$rustcVersion = (& rustc --version) -replace '^rustc ', ''
if ($rustcVersion -notlike '1.97.1*') {
    Write-Warning "Local rustc is $rustcVersion; CI pins 1.97.1. Build proceeds with the local toolchain."
}

# 程式內自動更新簽章金鑰（加密格式，需要密碼）。
$keyPath = Join-Path $env:USERPROFILE '.tauri/roudamix.key'
if ($env:TAURI_SIGNING_PRIVATE_KEY) {
    Write-Host 'Using TAURI_SIGNING_PRIVATE_KEY from environment.'
} elseif ((Test-Path -LiteralPath $keyPath) -and (Test-Path -LiteralPath "$keyPath.pub")) {
    $confPub = (Get-Content src-tauri/tauri.conf.json -Raw | ConvertFrom-Json).plugins.updater.pubkey
    $filePub = (Get-Content "$keyPath.pub" -Raw).Trim()
    if ($filePub -ne $confPub) { throw "Updater pubkey mismatch between $keyPath.pub and src-tauri/tauri.conf.json" }
    Write-Host "Using signing key: $keyPath"
} else {
    throw "Missing updater signing key: set TAURI_SIGNING_PRIVATE_KEY or provide $keyPath (+ .pub)"
}

# --- 版本與衝突驗證（對齊 CI 的 Validate release version and target）---
Invoke-ReleaseCommand python @('scripts/release.py', 'prepare', '--version', $Version)
$meta = Get-Content target/release-meta/build.json -Raw | ConvertFrom-Json
$tag = $meta.tag
$repo = gh repo view --json nameWithOwner -q .nameWithOwner
if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve GitHub repository' }
# gh release view returns non-zero for a missing release; drafts visible to collaborators.
if (Test-CommandResult { gh release view $tag --repo $repo > $null 2>&1 }) {
    throw "Release $tag already exists; choose a new version or delete the old release."
}
$tagCommit = $null
if (Test-CommandResult { $script:tagCommit = git rev-parse --verify "refs/tags/$tag^{commit}" 2>$null }) {
    if ($tagCommit -ne $meta.commit) { throw "Existing tag $tag points to a different commit" }
}
$remoteTags = git ls-remote --tags origin "refs/tags/$tag"
if ($remoteTags -and -not ($remoteTags -match [regex]::Escape($meta.commit))) {
    throw "Remote tag $tag points to a different commit"
}

if ($ValidateOnly) {
    Write-Host "Preflight OK: $tag @ $($meta.commit)"
    exit 0
}

# --- 建置 + 完整測試 + 打包（= CI build job 的 Test, build portable... 步驟）---
$env:TAURI_SIGNING_PRIVATE_KEY = Get-Content -LiteralPath $keyPath -Raw
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($KeyPassword)
try { $env:TAURI_SIGNING_PRIVATE_KEY_PASSWORD = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr) }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', 'scripts/build-release.ps1', '-Version', $Version)
if ($SkipTests) { $buildArgs += '-SkipTests' }
Invoke-ReleaseCommand powershell $buildArgs

# --- 資產完整性（= CI publish job 的 sha256sum --check 與必備檔案檢查）---
$assetRoot = Join-Path $repoRoot 'output/release'
foreach ($line in Get-Content (Join-Path $assetRoot 'SHA256SUMS.txt')) {
    if (-not $line.Trim()) { continue }
    $parts = $line -split '  ', 2
    $actual = (Get-FileHash -LiteralPath (Join-Path $assetRoot $parts[1]) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $parts[0].ToLowerInvariant()) { throw "SHA256 mismatch: $($parts[1])" }
}
foreach ($suffix in @('windows-x64-portable.zip', 'windows-x64-setup.exe', 'source.tar.gz', 'latest.json', 'windows-x64-setup.exe.sig')) {
    $name = if ($suffix -eq 'latest.json') { $suffix } else { "RoudaMix-$Version-$suffix" }
    if (-not (Test-Path -LiteralPath (Join-Path $assetRoot $name))) { throw "Missing release asset: $name" }
}

# --- 建 tag、推送、發布（= CI publish job；release.py prepare 記錄的 HEAD 即發布提交）---
if (-not (Test-CommandResult { git rev-parse --verify "refs/tags/$tag" 2>$null })) {
    git tag -a $tag -m "RoudaMix $Version" $meta.commit
    if ($LASTEXITCODE -ne 0) { throw "git tag $tag failed" }
}
git push origin HEAD
if ($LASTEXITCODE -ne 0) { throw 'git push (branch) failed' }
git push origin "refs/tags/$tag"
if ($LASTEXITCODE -ne 0) { throw "git push (tag $tag) failed" }

$createArgs = @('release', 'create', $tag, '--draft', '--target', $meta.commit,
    '--title', "RoudaMix $Version", '--notes-file', 'target/release-meta/notes.md')
if ($Version -match '-') { $createArgs += '--prerelease' }
$createArgs += (Get-ChildItem -LiteralPath $assetRoot -File | ForEach-Object { $_.FullName })
Invoke-ReleaseCommand gh $createArgs

if ($Draft) {
    Write-Host "Draft created. After inspection, publish with: gh release edit $tag --draft=false"
} else {
    gh release edit $tag --draft=false
    if ($LASTEXITCODE -ne 0) { throw 'Failed to publish (un-draft) the release' }
    Write-Host "Release published: https://github.com/$repo/releases/tag/$tag"
}
