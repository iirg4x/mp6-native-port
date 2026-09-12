param(
  [ValidateSet('launch','resume','measure','sample','capture','stop')][string]$Action,
  [ValidatePattern('^[a-z0-9-]+$')][string]$Name,
  [ValidateSet(0,1,2)][int]$Ao = 0,
  [ValidateRange(10,60)][int]$Seconds = 20,
  [switch]$Warmup
)
$ErrorActionPreference = 'Stop'
# Actual Release native code, normal 60 Hz simulation plus unlocked rendering.
# Isolated saves/cache only. A paused window leaves the board loaded to cool.
$Cohort = 0
$ApLimit = 37.3
$SkinLimit = 36.3
if ($Warmup) {
  # Warm-up is never FPS evidence. Status 2 still stops the run immediately.
  $Cohort = 1
  $ApLimit = 42.0
  $SkinLimit = 39.0
}
& python tests/integration/android_device_profile.py $Action --serial RZCTB00SF3W --model SM-S906E --session s22-fifo-packets-20260912 --name $Name --ao $Ao --fxaa 1 --vsync 0 --tick-hz 60 --shadow-quality 4 --ticks 18000 --max-start-status $Cohort --max-start-ap-c $ApLimit --max-start-skin-c $SkinLimit --max-window-status $Cohort --seconds $Seconds --pause
if ($LASTEXITCODE -ne 0) { throw 'Owned packet-writer phone run failed' }
