# Measures GPU engine utilisation of Fringeless (and of dwm.exe, which
# composites its overlay) while tests/workload.ps1 generates desktop activity.
#   .\tests\measure-gpu.ps1 -Exe build\Fringeless.exe
param([Parameter(Mandatory)] [string]$Exe, [int]$Samples = 10)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class AppWin {
    [DllImport("user32", CharSet = CharSet.Unicode)] static extern IntPtr FindWindowW(string c, string t);
    [DllImport("user32")] static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static bool Quit() { var h = FindWindowW("FringelessPanel", null); return h != IntPtr.Zero && PostMessageW(h, 0x8003, IntPtr.Zero, IntPtr.Zero); }
}
'@

function GpuPercent([int]$procId) {
    $sets = Get-Counter "\GPU Engine(pid_$($procId)_*)\Utilization Percentage" -SampleInterval 1 -MaxSamples $Samples -ErrorAction SilentlyContinue
    $per = foreach ($s in $sets) { ($s.CounterSamples | Measure-Object CookedValue -Sum).Sum }
    [math]::Round(($per | Measure-Object -Average).Average, 1)
}

$app = Start-Process (Resolve-Path $Exe) -PassThru
Start-Sleep -Seconds 4
$dwm = (Get-Process dwm | Select-Object -First 1).Id
$result = foreach ($mode in 'idle', 'small', 'large') {
    $load = $null
    if ($mode -ne 'idle') {
        $load = Start-Process pwsh -ArgumentList '-NoProfile', '-File', "$root\tests\workload.ps1", '-Mode', $mode, '-Seconds', ($Samples + 6) -PassThru -WindowStyle Hidden
        Start-Sleep -Seconds 3
        $visible = (Get-Process -Id $load.Id).MainWindowTitle
        if (-not $visible) { Write-Warning "workload window for '$mode' is not visible" }
    }
    $job = Start-Job { param($id, $n) $s = Get-Counter "\GPU Engine(pid_$($id)_*)\Utilization Percentage" -SampleInterval 1 -MaxSamples $n -ErrorAction SilentlyContinue; [math]::Round((($s | ForEach-Object { ($_.CounterSamples | Measure-Object CookedValue -Sum).Sum }) | Measure-Object -Average).Average, 1) } -ArgumentList $dwm, $Samples
    $fs = GpuPercent $app.Id
    $dw = Receive-Job $job -Wait -AutoRemoveJob
    if ($load) { $load.WaitForExit(); }
    [pscustomobject]@{ Scenario = $mode; AppGpu = $fs; DwmGpu = $dw }
}
[AppWin]::Quit() | Out-Null
Start-Sleep -Seconds 2
if (-not $app.HasExited) { Stop-Process -Id $app.Id }
$result | Format-Table -AutoSize
