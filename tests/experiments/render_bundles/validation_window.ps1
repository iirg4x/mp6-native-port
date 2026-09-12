param([ValidateSet(0,1,2)][int]$Ao = 1, [ValidateSet(0,1,2,3)][int]$Aa = 2)
$ErrorActionPreference = 'Stop'
$ValidationName = 'render-bundles-validation-ao' + $Ao + '-aa' + $Aa
& python tests/integration/run_board_qa.py --exe build/render-bundles-20260912/windows-validation/release/mp6native.exe --name $ValidationName --seconds 60 --ticks 120000 --stop-round 0 --widescreen --window-size 1920x1080 --ao $Ao --aa $Aa --cache-seed build/board-qa-runs/compact-instance-board-off/gpu-cache --input-script 'period:30;timeout:119000;pressuntil:a/w01.live/120;wait:18000' --capture-trigger w01.live --capture-delay 1800 --capture-frames 6 --capture-stride 150
if ($LASTEXITCODE -ne 0) { throw 'Private Dawn validation test failed' }
