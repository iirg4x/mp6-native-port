param(
  [ValidateSet('launch','resume','measure','sample','capture','stop')][string]$Action,
  [ValidatePattern('^[a-z0-9-]+$')][string]$Name,
  [ValidateSet(0,1)][int]$Ao = 0,
  [ValidateSet(0,1)][int]$Cohort = 0,
  [ValidateRange(10,60)][int]$Seconds = 25,
  [switch]$FastForward
)
$ErrorActionPreference = 'Stop'
$AbbaHz = 60
if ($FastForward) { $AbbaHz = 0 }
# Cohort 1 is a separately labelled, steady-light-thermal diagnostic. It is
# never described as cool or merged with cohort 0. Status 2 always stops a run.
& python tests/integration/android_device_profile.py $Action --serial RZCTB00SF3W --model SM-S906E --session s22-bundle-abba-20260912 --name $Name --ao $Ao --fxaa 1 --vsync 0 --tick-hz $AbbaHz --shadow-quality 4 --ticks 24000 --max-start-status $Cohort --max-start-ap-c 38.0 --max-start-skin-c 37.0 --max-window-status $Cohort --seconds $Seconds --pause
if ($LASTEXITCODE -ne 0) { throw 'Owned ABBA device command failed' }
