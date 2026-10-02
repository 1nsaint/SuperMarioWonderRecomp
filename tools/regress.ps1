# Windows regression suite (docs/REGRESSION.md): the same measured runs on two builds, then a verdict.
#
#   tools\regress.ps1 -Builds "base=private\wonder\regress\bin\wonder-base.exe,merged=private\wonder\regress\bin\wonder-merged.exe"
#   tools\regress.ps1 -Compare base,merged        the table and the verdict (exit code 1 on a regression)
#
# The builds run interleaved (boot base, boot merged, ...; course base, course merged, ...), so whatever else loads the
# laptop (Parsec, another game, the antivirus) weighs on both alike. Per build: boots (headless, 25 s each) and the
# scripted course run (tools\wonder_course.ps1) on the dedicated GPU and on the integrated one, -Repeat times each.
# Unit tests (ctest) run on the build in private\wonder\build, and count for the last label. Results:
# private\wonder\regress\<label>\results.json and each run's folder.
param(
    [string]$Builds = "",
    [string[]]$Compare = @(),
    [int]$Boots = 5,
    [int]$Repeat = 2,
    [int]$Seconds = 180,
    [switch]$NoIntegrated
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$out = "private\wonder\regress"

function Percentile($values, $p) {
    $s = @($values | Sort-Object)
    if (-not $s.Count) { return 0 }
    return $s[[int][math]::Floor(($s.Count - 1) * $p)]
}

# Numbers from one run folder (tools\wonder_run.ps1 layout): fps windows after the first 20 (boot and menus vary).
function RunStats($dir) {
    $log = if (Test-Path "$dir\nx.log") { Get-Content "$dir\nx.log" } else { @() }
    $fps = @($log | Select-String 'per frame over 60: ([\d.]+) fps' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } |
        Where-Object { $_ -gt 0 } | Select-Object -Skip 20)
    $replay = @($log | Select-String 'replay ([\d.]+) ms' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Select-Object -Skip 20)
    $mem = @($log | Select-String 'memory: [\d.]+ GB \(peak ([\d.]+) GB' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
    $glitch = 0
    if (Test-Path "$dir\frames") {
        $g = & private\venv\Scripts\python tools\find_glitches.py "$dir\frames" 20 | Select-String '(\d+) frames flagged'
        if ($g) { $glitch = [int]$g.Matches[0].Groups[1].Value }
    }
    [ordered]@{
        fps_median = Percentile $fps 0.5; fps_p10 = Percentile $fps 0.1; replay_ms_median = Percentile $replay 0.5
        mem_peak_gb = if ($mem.Count) { ($mem | Measure-Object -Maximum).Maximum } else { 0 }
        crashes = @($log | Select-String 'CRASH|GPU hang|over the .* limit').Count
        vk_errors = @($log | Select-String 'nvn/vk: .* failed \(').Count; glitch_frames = $glitch; windows = $fps.Count
    }
}

function Summary($runs) {
    [ordered]@{
        fps_median = Percentile ($runs | ForEach-Object { $_.fps_median }) 0.5
        fps_p10 = ($runs | ForEach-Object { $_.fps_p10 } | Measure-Object -Minimum).Minimum
        replay_ms_median = Percentile ($runs | ForEach-Object { $_.replay_ms_median }) 0.5
        mem_peak_gb = ($runs | ForEach-Object { $_.mem_peak_gb } | Measure-Object -Maximum).Maximum
        crashes = ($runs | ForEach-Object { $_.crashes } | Measure-Object -Sum).Sum
        vk_errors = ($runs | ForEach-Object { $_.vk_errors } | Measure-Object -Sum).Sum
        glitch_frames = ($runs | ForEach-Object { $_.glitch_frames } | Measure-Object -Sum).Sum
        runs = $runs
    }
}

function Interleave($spec) {
    $builds = [ordered]@{}
    foreach ($pair in $spec -split ',') { $k, $v = $pair -split '=', 2; $builds[$k.Trim()] = $v.Trim() }
    $res = [ordered]@{}
    foreach ($l in $builds.Keys) {
        New-Item -ItemType Directory -Force "$out\$l" | Out-Null
        $res[$l] = [ordered]@{ label = $l; exe = $builds[$l]; when = (Get-Date -Format s); boot_failures = 0; unit_failed = 0 }
    }
    # Unit tests of the build in private\wonder\build (the candidate's), counted for every label.
    . .\dev.ps1 *> $null
    $ct = ctest --test-dir private\wonder\build 2>&1 | Select-String 'tests passed, (\d+) tests failed out of (\d+)'
    $unit = if ($ct) { [int]$ct.Matches[0].Groups[1].Value } else { -1 }
    foreach ($l in $builds.Keys) { $res[$l].unit_failed = $unit }
    # Boots: a crash or no frame within 25 s fails.
    for ($k = 1; $k -le $Boots; $k++) {
        foreach ($l in $builds.Keys) {
            $name = "regress-$l-boot$k"
            & "$PSScriptRoot\wonder_run.ps1" -Name $name -Gpu discrete -Seconds 25 -Headless -MinPresents 60 -Exe $builds[$l] 6>$null | Out-Null
            $v = Get-Content "private\wonder\runs\$name\summary.txt" -ErrorAction SilentlyContinue | Select-String 'verdict'
            if ("$v" -notmatch 'PASS') { $res[$l].boot_failures++; Write-Host "boot $k $l : $v" }
            Remove-Item -Recurse -Force "private\wonder\runs\$name"
        }
    }
    $gpus = @("discrete") + $(if ($NoIntegrated) { @() } else { @("integrated") })
    $runs = @{}
    foreach ($gpu in $gpus) {
        for ($k = 1; $k -le $Repeat; $k++) {
            foreach ($l in $builds.Keys) {
                $name = "regress-$l-$gpu-$k"
                $delay = if ($gpu -eq "integrated") { 8 } else { 0 }
                & "$PSScriptRoot\wonder_course.ps1" -Name $name -Gpu $gpu -Seconds $Seconds -DumpEvery 600 -Delay $delay -Exe $builds[$l] 6>$null | Out-Null
                $st = RunStats "private\wonder\runs\$name"
                Write-Host ("{0} {1} {2}: fps median {3}, p10 {4}, replay {5} ms, glitches {6}" -f $gpu, $l, $k, $st.fps_median, $st.fps_p10, $st.replay_ms_median, $st.glitch_frames)
                if (-not $runs["$l/$gpu"]) { $runs["$l/$gpu"] = @() }
                $runs["$l/$gpu"] += $st
                if (Test-Path "$out\$l\$name") { Remove-Item -Recurse -Force "$out\$l\$name" }
                Move-Item "private\wonder\runs\$name" "$out\$l\$name"
            }
        }
    }
    foreach ($l in $builds.Keys) {
        foreach ($gpu in $gpus) { $res[$l][$gpu] = Summary $runs["$l/$gpu"] }
        $res[$l] | ConvertTo-Json -Depth 6 | Set-Content -Encoding utf8 "$out\$l\results.json"
        "$l done: $out\$l\results.json"
    }
}

# Base against candidate. A loss: unit tests or boots failing, a crash, more glitch frames, median fps down more than
# 3% (p10 more than 10%), replay CPU up more than 10% (+0.3 ms), peak memory up more than 10%.
function CompareRuns($a, $b) {
    $A = Get-Content "$out\$a\results.json" -Raw | ConvertFrom-Json
    $B = Get-Content "$out\$b\results.json" -Raw | ConvertFrom-Json
    if (Test-Path "$out\$a\phone.json") { $A | Add-Member -NotePropertyName phone -NotePropertyValue (Get-Content "$out\$a\phone.json" -Raw | ConvertFrom-Json) -Force }
    if (Test-Path "$out\$b\phone.json") { $B | Add-Member -NotePropertyName phone -NotePropertyValue (Get-Content "$out\$b\phone.json" -Raw | ConvertFrom-Json) -Force }
    $script:loss = @()
    $script:rows = @()
    $row = { param($what, $x, $y, $bad) $script:rows += "{0,-34} {1,10} {2,10}  {3}" -f $what, $x, $y, $(if ($bad) { 'LOSS' } else { 'ok' }); if ($bad) { $script:loss += $what } }
    & $row "unit tests failed" $A.unit_failed $B.unit_failed ($B.unit_failed -ne 0)
    & $row "boot failures" $A.boot_failures $B.boot_failures ($B.boot_failures -gt $A.boot_failures)
    foreach ($gpu in "discrete", "integrated", "phone") {
        if (-not $A.$gpu -or -not $B.$gpu) { continue }
        $x = $A.$gpu; $y = $B.$gpu
        & $row "$gpu fps median" $x.fps_median $y.fps_median ($y.fps_median -lt $x.fps_median * 0.97)
        & $row "$gpu fps p10" $x.fps_p10 $y.fps_p10 ($y.fps_p10 -lt $x.fps_p10 * 0.90)
        & $row "$gpu replay ms (median)" $x.replay_ms_median $y.replay_ms_median ($y.replay_ms_median -gt $x.replay_ms_median * 1.10 + 0.3)
        & $row "$gpu memory peak GB" $x.mem_peak_gb $y.mem_peak_gb ($y.mem_peak_gb -gt $x.mem_peak_gb * 1.10)
        & $row "$gpu crashes/hangs" $x.crashes $y.crashes ($y.crashes -gt 0)
        & $row "$gpu Vulkan errors" $x.vk_errors $y.vk_errors ($y.vk_errors -gt $x.vk_errors)
        & $row "$gpu glitch frames" $x.glitch_frames $y.glitch_frames ($y.glitch_frames -gt $x.glitch_frames)
    }
    "{0,-34} {1,10} {2,10}" -f "", $a, $b
    $script:rows
    if ($script:loss.Count) { "VERDICT: LOSS in " + ($script:loss -join ", "); exit 1 }
    "VERDICT: no loss"
    exit 0
}

if ($Compare.Count -eq 2) { CompareRuns $Compare[0] $Compare[1] }
elseif ($Builds) { Interleave $Builds }
else { Write-Host 'usage: tools\regress.ps1 -Builds "base=<exe>,merged=<exe>" | -Compare <base>,<candidate>'; exit 2 }
