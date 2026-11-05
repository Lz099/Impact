param([string]$Label='equipment_play1')
$ErrorActionPreference='Stop'
$Project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if($Label -notmatch '^[a-zA-Z0-9_-]+$'){throw 'Invalid label'}
$Output=Join-Path $Project ('Saved/Equipment/'+$Label)
if(Test-Path -LiteralPath $Output){throw 'Evidence already exists; choose another label.'}
New-Item -ItemType Directory -Path $Output | Out-Null
$Captures=Join-Path $Project 'Saved/Screenshots/WindowsEditor'
$Archive=Join-Path $Output 'previous'
New-Item -ItemType Directory -Path $Archive | Out-Null
Get-ChildItem -LiteralPath $Captures -Filter 'auto_*.png' -ErrorAction SilentlyContinue | Copy-Item -Destination $Archive
$Started=Get-Date
$Arguments=@((Join-Path $Project 'Impact.uproject'),'/ALS/ALSExtras/Levels/L_Als_Playground','-game','-RenderOffScreen','-unattended','-nosound','-resx=1280','-resy=720','-ImpactEquipmentBench','-ImpactShots=8,12,16,20,24,28,32,36,40,44','-ImpactQuit=46',('-abslog='+(Join-Path $Output 'game.log')))
$Process=Start-Process 'D:/Unreal/UE_5.7/Engine/Binaries/Win64/UnrealEditor.exe' -ArgumentList $Arguments -WindowStyle Hidden -PassThru
Write-Output "Equipment test instance $($Process.Id)"
if(!$Process.WaitForExit(360000)){Stop-Process -Id $Process.Id;throw 'Own test instance timed out.'}
Get-ChildItem -LiteralPath $Captures -Filter 'auto_*.png' | Where-Object LastWriteTime -ge $Started | Copy-Item -Destination $Output
if($Process.ExitCode -ne 0){throw "Unreal exited with $($Process.ExitCode)"}
Write-Output "Evidence: $Output"
