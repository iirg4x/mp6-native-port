param(
  [ValidateSet('launch','resume','measure','sample','capture','stop')][string]$Action,
  [ValidatePattern('^[a-z0-9-]+$')][string]$Name,
  [ValidateSet(0,1,2)][int]$Ao = 0,
  [ValidateRange(10,60)][int]$Seconds = 15,
  [switch]$FastForward,
  [switch]$Smoke
)
$ErrorActionPreference = 'Stop'
$BundleTickHz = 60
if ($FastForward) { $BundleTickHz = 0 }
$BundleStatus = 0
$BundleAp = 37.3
$BundleSkin = 36.3
if ($Smoke) {
  # Explicit correctness-only run, never admitted to a cool FPS comparison.
  $BundleStatus = 1
  $BundleAp = 38.0
  $BundleSkin = 37.0
}
& python tests/integration/android_device_profile.py $Action --serial RZCTB00SF3W --model SM-S906E --session s22-render-bundles-20260912 --name $Name --ao $Ao --fxaa 1 --vsync 0 --tick-hz $BundleTickHz --shadow-quality 4 --ticks 24000 --max-start-status $BundleStatus --max-start-ap-c $BundleAp --max-start-skin-c $BundleSkin --max-window-status $BundleStatus --seconds $Seconds --pause
if ($LASTEXITCODE -ne 0) { throw 'Owned bundle phone run failed' }
