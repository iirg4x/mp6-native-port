param([ValidateSet(0,1)][int]$Ao = 0, [ValidateSet(0,1)][int]$Mode = 1,
      [ValidatePattern('^r[0-9]+$')][string]$Revision = 'r1', [ValidateSet(2,3,4)][int]$Aa = 2)
$ErrorActionPreference = 'Stop'
$UploadRunName = "upload-batch-$Revision-ao$Ao-mode$Mode"
if ($Aa -ne 2) { $UploadRunName += "-aa$Aa" }
$UploadExe = "build/upload-batch-20260912/windows-$Revision/release/mp6native.exe"
& python tests/integration/run_board_qa.py --exe $UploadExe --name $UploadRunName --seconds 40 --ticks 120000 --stop-round 0 --widescreen --window-size 1920x1080 --ao $Ao --aa $Aa --frame-upload-mode $Mode --cache-seed build/board-qa-runs/compact-instance-board-off/gpu-cache --input-script 'period:30;timeout:119000;pressuntil:a/w01.live/120;wait:18000' --capture-trigger w01.live --capture-delay 1800 --capture-frames 6 --capture-stride 150
if ($LASTEXITCODE -ne 0) { throw 'Private upload scheduling board check failed' }
