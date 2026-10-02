# The scripted course run (docs/nvn/WINDOWS-PLAN.md, W3): a new game from the title to course 1-1 ("Welcome to the
# Flower Kingdom!"), then right along the course, jumping. Frames every -DumpEvery presents for comparison with the
# Mac's docs/media/wonder/33-39 and between GPUs.
#
#   tools\wonder_course.ps1 -Name course-dgpu                       RTX 3050, docked, headless
#   tools\wonder_course.ps1 -Name course-igpu -Gpu integrated -Handheld
#
# The timeline (seconds after the first pad read): L+R at the title (20), A through user, players and character
# select and the course's first dialog (26-50, every 3 s), then RIGHT held with a jump every 1.5 s. Slower GPUs
# load slower: -Delay shifts everything after the title (the UHD 770 needs about 10).
param(
    [string]$Name = "course",
    [string]$Gpu = "discrete",
    [int]$Seconds = 180,
    [int]$DumpEvery = 600,
    [int]$Delay = 0,
    [switch]$Handheld,
    [switch]$Windowed,
    [switch]$Course11,                   # go on through the opening and the map into course 1-1 (-Seconds 320)
    [string]$Env = "",
    [double]$MinFps = 0,
    [string]$Exe = "",                   # another build (tools\regress.ps1 runs two side by side)
    [switch]$KeepSave                    # keep private\wonder\user\save (default: a new game)
)
$s = "20:L:0.4;20:R:0.4"
for ($t = 26; $t -le 50; $t += 3) { $s += ";$($t + $Delay):A:0.25" }
$walk = 55 + $Delay
if ($Course11) {
    # Through the opening (walk until 140 s), its cutscenes and the map's first node (A every 3 s until 200 s: the
    # map starts on course 1-1), then course 1-1 itself: RIGHT with a jump every 1.5 s.
    $stop = 140 + $Delay
    $s += ";${walk}:RIGHT:$($stop - $walk)"
    for ($t = $walk + 1; $t -lt $stop; $t += 1.5) { $s += ";${t}:A:0.35" }
    for ($t = $stop + 5; $t -le 200 + $Delay; $t += 3) { $s += ";${t}:A:0.25" }
    $walk = 205 + $Delay
}
$s += ";${walk}:RIGHT:$($Seconds - $walk)"
for ($t = $walk + 1; $t -lt $Seconds; $t += 1.5) { $s += ";${t}:A:0.35" }
$run = @{ Name = $Name; Gpu = $Gpu; Seconds = $Seconds; DumpEvery = $DumpEvery; Script = $s; Env = $Env;
           MaxMemGB = 6; MinFps = $MinFps }
if ($Handheld) { $run.Handheld = $true }
if ($Exe) { $run.Exe = $Exe }
if ($KeepSave) { $run.KeepSave = $true }
if (-not $Windowed) { $run.Headless = $true }
& "$PSScriptRoot\wonder_run.ps1" @run
exit $LASTEXITCODE
