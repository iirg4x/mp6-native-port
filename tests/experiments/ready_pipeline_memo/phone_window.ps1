param(
  [ValidateSet('launch','resume','measure','sample','capture','stop')][string]$Action,
  [ValidatePattern('^[a-z0-9-]+$')][string]$Name,
  [ValidateSet(0,1,2)][int]$Ao = 0,
  [ValidateRange(10,60)][int]$Seconds = 25,
  [switch]$Warmup,
  [switch]$FastForward
)
$ErrorActionPreference = 'Stop'
$Cohort = 0
$ApLimit = 37.3
$SkinLimit = 36.3
$TickHz = 60
if ($FastForward) { $TickHz = 0 }
if ($Warmup) {
  $Cohort = 1
  $ApLimit = 42.0
  $SkinLimit = 39.0
}
& python tests/integration/android_device_profile.py $Action --serial RZCTB00SF3W --model SM-S906E --session s22-ready-memo-20260912 --name $Name --ao $Ao --fxaa 1 --vsync 0 --tick-hz $TickHz --shadow-quality 4 --ticks 18000 --max-start-status $Cohort --max-start-ap-c $ApLimit --max-start-skin-c $SkinLimit --max-window-status $Cohort --seconds $Seconds --pause
if ($LASTEXITCODE -ne 0) { throw 'Owned ready-memo phone run failed' }
