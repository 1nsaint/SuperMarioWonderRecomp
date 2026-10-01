# One unattended Wonder run on Windows, and its verdict (docs/nvn/WINDOWS-PLAN.md, "Test plan").
#
#   tools\wonder_run.ps1 -Name title-dgpu -Gpu discrete -Seconds 90 -Headless -DumpEvery 300
#   tools\wonder_run.ps1 -Name course-igpu -Gpu integrated -Seconds 240 -Script (Get-Content tools\wonder_course.txt)
#
# Writes private\wonder\runs\<Name>\: nx.log, frames\*.png (NX_DUMP_FRAMES), summary.txt. Exit code 0 when the run
# passes: no crash or GPU hang, at least -MinPresents presents, peak memory under -MaxMemGB and, when -MinFps is
# given, a median fps at or above it. One game process at a time, memory guard on (NX_MEM_LIMIT_GB).
param(
    [Parameter(Mandatory)][string]$Name,
    [string]$Gpu = "discrete",           # NX_GPU: discrete | integrated | <index> | <name part>
    [int]$Seconds = 90,
    [switch]$Headless,
    [switch]$Handheld,                   # NX_HANDHELD=1: 720p
    [string]$Script = "",                # NX_INPUT_SCRIPT
    [int]$DumpEvery = 0,                 # NX_DUMP_FRAMES
    [string]$Env = "",                   # any other switches: "NX_A=1;NX_B=2"
    [int]$MinPresents = 300,
    [double]$MinFps = 0,
    [double]$MaxMemGB = 5.0,
    [string]$Exe = "private\wonder\build\wonder.exe",
    [switch]$KeepSave                    # default: the save is deleted first, so scripted input meets the same screens
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
if (Get-Process wonder -ErrorAction SilentlyContinue) { Write-Error "a wonder process is already running"; exit 2 }

$run = "private\wonder\runs\$Name"
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
New-Item -ItemType Directory -Force "$run\frames" | Out-Null
$frames = "private\wonder\frames"
if (Test-Path $frames) { Remove-Item -Recurse -Force $frames }
if (-not $KeepSave -and (Test-Path "private\wonder\user\save")) { Remove-Item -Recurse -Force "private\wonder\user\save" }

$vars = @{ NX_LOG = "$run\nx.log"; NX_MEM_LIMIT_GB = "8"; NX_GPU = $Gpu }
if ($Headless) { $vars.NX_HEADLESS = "1"; $vars.NX_MUTE = "1" }
if ($Handheld) { $vars.NX_HANDHELD = "1" }
if ($Script) { $vars.NX_INPUT_SCRIPT = $Script }
if ($DumpEvery) { $vars.NX_DUMP_FRAMES = "$DumpEvery" }
foreach ($kv in ($Env -split ';' | Where-Object { $_ -match '=' })) { $k, $v = $kv -split '=', 2; $vars[$k.Trim()] = $v }
$saved = @{}
foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $vars[$k]) }

$t0 = Get-Date
try {
    $p = Start-Process -FilePath $Exe -PassThru -RedirectStandardError "$run\stderr.txt" -WindowStyle Normal
    $exited = $p.WaitForExit($Seconds * 1000)
    if (-not $exited) { Stop-Process -Id $p.Id -Force; $p.WaitForExit() }
    $code = if ($exited) { $p.ExitCode } else { "killed after $Seconds s (expected)" }
} finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}
$elapsed = [int]((Get-Date) - $t0).TotalSeconds

# Frames: PPM -> PNG (tools\frames.py), into the run folder.
if (Test-Path $frames) {
    & private\venv\Scripts\python tools\frames.py png $frames "$run\frames" | Out-Null
    Remove-Item -Recurse -Force $frames
}

# Verdict from the log.
$log = if (Test-Path "$run\nx.log") { Get-Content "$run\nx.log" } else { @() }
$fps = @($log | Select-String 'per frame over \d+: ([\d.]+) fps' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } |
    Where-Object { $_ -gt 0 })
$sorted = @($fps | Sort-Object)
$median = if ($sorted.Count) { $sorted[[int][math]::Floor($sorted.Count / 2)] } else { 0 }
$low = if ($sorted.Count) { $sorted[0] } else { 0 }
$presents = 60 * $fps.Count
$mem = @($log | Select-String 'memory: ([\d.]+) GB \(peak ([\d.]+) GB' | ForEach-Object { [double]$_.Matches[0].Groups[2].Value })
$peak = if ($mem.Count) { ($mem | Measure-Object -Maximum).Maximum } else { 0 }
$crash = @($log | Select-String 'CRASH|GPU hang|fatal|over the .* limit')
$vkerr = @($log | Select-String 'nvn/vk: .* failed \(' )
$gpuLine = ($log | Select-String 'nvn/vk: using GPU' | Select-Object -First 1)
$present = ($log | Select-String 'nvn/vk: present mode' | Select-Object -First 1)

$fail = @()
if ($crash.Count) { $fail += "crash/hang: $($crash[0].Line)" }
if ($presents -lt $MinPresents) { $fail += "presents $presents < $MinPresents" }
if ($peak -gt $MaxMemGB) { $fail += "peak memory $peak GB > $MaxMemGB" }
if ($MinFps -gt 0 -and $median -lt $MinFps) { $fail += "median fps $median < $MinFps" }

$summary = @(
    "run        $Name ($(if ($Headless) {'headless'} else {'windowed'})$(if ($Handheld) {', handheld'}), $Seconds s, $elapsed s wall)",
    "gpu        $(if ($gpuLine) { $gpuLine.Line -replace '.*using GPU ', '' } else { '?' })",
    "present    $(if ($present) { $present.Line -replace '.*present mode ', '' } else { '-' })",
    "exit       $code",
    "presents   ~$presents",
    ("fps        median {0:N1}, lowest {1:N1} over {2} windows of 60 frames" -f $median, $low, $fps.Count),
    ("memory     peak {0:N2} GB" -f $peak),
    "vk errors  $($vkerr.Count)$(if ($vkerr.Count) { ': ' + $vkerr[0].Line })",
    "frames     $((Get-ChildItem "$run\frames" -Filter *.png -ErrorAction SilentlyContinue).Count)",
    "verdict    $(if ($fail.Count) { 'FAIL: ' + ($fail -join '; ') } else { 'PASS' })"
)
$summary | Set-Content -Encoding utf8 "$run\summary.txt"
$summary | ForEach-Object { Write-Host $_ }
exit $(if ($fail.Count) { 1 } else { 0 })
