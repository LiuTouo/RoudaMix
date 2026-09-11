[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$WorkerPath)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
public static class WorkerErrorUiProcess {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct StartupInfo {
        public uint cb; public string reserved, desktop, title;
        public uint x, y, xSize, ySize, xChars, yChars, fill, flags;
        public ushort show, reservedSize; public IntPtr reservedBytes, input, output, error;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInfo { public IntPtr process, thread; public uint pid, tid; }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern bool CreateProcess(string app, StringBuilder args, IntPtr pa, IntPtr ta,
        bool inherit, uint flags, IntPtr env, string cwd, ref StartupInfo si, out ProcessInfo pi);
    [DllImport("kernel32.dll")] static extern bool SetHandleInformation(IntPtr h, uint mask, uint flags);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h, out uint code);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h, uint code);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static uint Run(string exe, string args, string output, string error) {
        using (var stdout = new FileStream(output, FileMode.Create, FileAccess.Write, FileShare.ReadWrite))
        using (var stderr = new FileStream(error, FileMode.Create, FileAccess.Write, FileShare.ReadWrite)) {
            var si = new StartupInfo(); si.cb = (uint)Marshal.SizeOf(si); si.flags = 0x100;
            si.output = stdout.SafeFileHandle.DangerousGetHandle();
            si.error = stderr.SafeFileHandle.DangerousGetHandle();
            if (!SetHandleInformation(si.output, 1, 1) || !SetHandleInformation(si.error, 1, 1))
                throw new Exception("Cannot inherit test output handles");
            ProcessInfo pi;
            // CREATE_DEFAULT_ERROR_MODE prevents the test runner from masking this bug.
            if (!CreateProcess(exe, new StringBuilder("\"" + exe + "\" " + args), IntPtr.Zero,
                IntPtr.Zero, true, 0x08000000 | 0x04000000, IntPtr.Zero, null, ref si, out pi))
                throw new Exception("CreateProcess failed: " + Marshal.GetLastWin32Error());
            CloseHandle(pi.thread);
            try {
                if (WaitForSingleObject(pi.process, 5000) != 0) {
                    TerminateProcess(pi.process, 99); WaitForSingleObject(pi.process, 5000);
                    throw new Exception("Worker blocked for 5 seconds; possible Windows error dialog");
                }
                uint code;
                if (!GetExitCodeProcess(pi.process, out code)) throw new Exception("Cannot read worker exit code");
                return code;
            } finally { CloseHandle(pi.process); }
        }
    }
}
'@
$workerExe = [IO.Path]::GetFullPath($WorkerPath)
if (-not (Test-Path -LiteralPath $workerExe -PathType Leaf)) { throw "Worker missing: $workerExe" }
# 確認可讀 System log，避免把讀取權限錯誤當成「沒有彈窗」。
$null = Get-WinEvent -ListLog System -ErrorAction Stop
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$caseRoot = Join-Path $tempRoot ('rmx-worker-error-ui-' + [guid]::NewGuid().ToString('N'))
$resolved = [IO.Path]::GetFullPath($caseRoot)
if (-not $resolved.StartsWith($tempRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Test directory is outside the temporary workspace'
}
New-Item -ItemType Directory -Path $caseRoot | Out-Null
$badModule = Join-Path $caseRoot 'invalid.vst3'
Set-Content -LiteralPath $badModule -Value 'deliberately invalid VST3 image' -Encoding ASCII
$started = Get-Date
try {
    foreach ($mode in @('verify', 'scan')) {
        $stdout = Join-Path $caseRoot "$mode.stdout"
        $stderr = Join-Path $caseRoot "$mode.stderr"
        $arguments = if ($mode -eq 'verify') { @('--verify', ('"' + $badModule + '"'), '48000', '512') }
                     else { @('--scan', ('"' + $caseRoot + '"')) }
        $exitCode = [WorkerErrorUiProcess]::Run($workerExe, ($arguments -join ' '), $stdout, $stderr)
        if ($mode -eq 'verify') {
            if ($exitCode -ne 1 -or (Get-Content -Raw $stderr) -notmatch 'load failed:') {
                throw "verify must report a load error (exit=$exitCode)"
            }
        } else {
            if ($exitCode -ne 0) { throw "scan failed (exit=$exitCode)" }
            $entries = @(Get-Content $stdout | ForEach-Object { $_ | ConvertFrom-Json })
            if ($entries.Count -ne 1 -or -not $entries[0].error) {
                throw 'scan must report the invalid module as an error entry'
            }
        }
    }
    # Hard-error popup 的 System / Application Popup 26 可能稍晚寫入。
    Start-Sleep -Seconds 1
    $dialogs = @(Get-WinEvent -FilterHashtable @{LogName='System';Id=26;StartTime=$started} `
        -ErrorAction SilentlyContinue | Where-Object { $_.Message.Contains([IO.Path]::GetFileName($caseRoot)) })
    if ($dialogs.Count -ne 0) { throw "Worker created $($dialogs.Count) Windows error dialog(s)" }
    Write-Host 'worker_error_ui PASSED: invalid plugins return errors without Windows dialogs'
} finally {
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
