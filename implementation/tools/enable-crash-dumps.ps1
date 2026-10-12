# Configure Windows local minidumps without changing game binaries.
# Reference: https://learn.microsoft.com/en-us/windows/win32/wer/collecting-user-mode-dumps
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run enable-crash-dumps.ps1 from an administrator PowerShell.'
}
$evidence = Join-Path $PSScriptRoot '..\evidence\crash-dump-configuration'
$null = New-Item -ItemType Directory -Path $evidence -Force
$base = [Microsoft.Win32.RegistryKey]::OpenBaseKey(
    [Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
$apps = @('HawkenGame-Win64-Shipping.exe', 'hawken-server.exe', 'hawken-master.exe')
$root = 'SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\'
$names = @('DumpFolder', 'DumpCount', 'DumpType')
try {
    $before = @()
    foreach ($app in $apps) {
        $key = $base.OpenSubKey($root + $app)
        try {
            $values = @()
            foreach ($name in $names) {
                if ($null -ne $key -and $key.GetValueNames() -contains $name) {
                    $values += @{ name=$name; kind=$key.GetValueKind($name).ToString(); value=$key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) }
                }
            }
            $before += @{ application=$app; keyExisted=($null -ne $key); values=$values }
        } finally { if ($null -ne $key) { $key.Dispose() } }
    }
    $backup = Join-Path $evidence 'before.json'
    if (-not (Test-Path -LiteralPath $backup)) {
        [IO.File]::WriteAllText($backup, ($before | ConvertTo-Json -Depth 6), [Text.UTF8Encoding]::new($false))
    }
    foreach ($app in $apps) {
        $key = $base.CreateSubKey($root + $app)
        try {
            $key.SetValue('DumpFolder', '%LOCALAPPDATA%\CrashDumps', [Microsoft.Win32.RegistryValueKind]::ExpandString)
            $key.SetValue('DumpCount', 10, [Microsoft.Win32.RegistryValueKind]::DWord)
            $key.SetValue('DumpType', 1, [Microsoft.Win32.RegistryValueKind]::DWord)
        } finally { $key.Dispose() }
        $key = $base.OpenSubKey($root + $app)
        try {
            if ($key.GetValue('DumpType') -ne 1 -or $key.GetValue('DumpCount') -ne 10 -or
                $key.GetValue('DumpFolder', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames) -ne '%LOCALAPPDATA%\CrashDumps') {
                throw "Crash dump configuration verification failed for $app."
            }
        } finally { $key.Dispose() }
    }
    $result = @{ schema='hawken.local-dumps.v1'; configured=$true; applications=$apps; dumpType=1; dumpCount=10; folder='%LOCALAPPDATA%\CrashDumps'; verifiedUtc=[DateTime]::UtcNow.ToString('o'); newCrashCaptureVerified=$false }
    [IO.File]::WriteAllText((Join-Path $evidence 'configured.json'), ($result | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    Write-Output 'Local minidumps configured and registry values verified.'
} finally { $base.Dispose() }
