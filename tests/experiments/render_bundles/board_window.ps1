param([ValidateSet(0,1)][int]$Ao = 0, [ValidatePattern('^r[0-9]+$')][string]$Revision = 'r1')
$ErrorActionPreference = 'Stop'
$BundleRunName = 'render-bundles-' + $Revision + '-' + $(if ($Ao -eq 0) { 'off' } else { 'ao1' })
$BundleExe = 'build/render-bundles-20260912/windows-' + $Revision + '/release/mp6native.exe'
& python tests/integration/run_board_qa.py --exe $BundleExe --name $BundleRunName --seconds 60 --ticks 120000 --stop-round 0 --widescreen --window-size 1920x1080 --ao $Ao --aa 2 --cache-seed build/board-qa-runs/compact-instance-board-off/gpu-cache --input-script 'period:30;timeout:119000;pressuntil:a/w01.live/120;wait:18000' --capture-trigger w01.live --capture-delay 1800 --capture-frames 6 --capture-stride 150
if ($LASTEXITCODE -ne 0) { throw 'Private render-bundle board test failed' }
