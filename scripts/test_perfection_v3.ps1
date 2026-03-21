$moonlightRoot = "C:\Users\beelink\Desktop\moonlight3"
$ppssppExe = "$moonlightRoot\PPSSPP_x64\PPSSPPWindows64.exe"
$ebootPath = "$moonlightRoot\psp_moonlight\EBOOT.PBP"
$moonlightLog1 = "$moonlightRoot\PPSSPP_x64\memstick\moonlight.log"
$moonlightLog2 = "$moonlightRoot\PPSSPP_x64\memstick\PSP\GAME\PSP_Moonlight\moonlight_debug.log"
$sunshineLog = "C:\Users\beelink\Applications\Files\Apollo\config\sunshine.log"

Write-Host "--- Starting Absolute Perfection Monitoring v3 ---" -ForegroundColor Cyan
Write-Host "Monitoring Moonlight 1: $moonlightLog1"
Write-Host "Monitoring Moonlight 2: $moonlightLog2"
Write-Host "Monitoring Sunshine: $sunshineLog"

# Clear old moonlight logs and keys to start fresh
if (Test-Path $moonlightLog1) { Remove-Item $moonlightLog1 -ErrorAction SilentlyContinue }
if (Test-Path $moonlightLog2) { Remove-Item $moonlightLog2 -ErrorAction SilentlyContinue }
$keysDir = "$moonlightRoot\PPSSPP_x64\memstick\moonlight\keys"
if (Test-Path $keysDir) { 
    Write-Host "--- Nuking stale keys in $keysDir ---" -ForegroundColor Red
    Remove-Item -Recurse -Force $keysDir -ErrorAction SilentlyContinue 
}
# Create empty log files so tail doesn't fail
New-Item -Path $moonlightLog1 -ItemType File -Force | Out-Null
New-Item -Path $moonlightLog2 -ItemType File -Force | Out-Null

Write-Host "--- Cleaning up Sunshine ---" -ForegroundColor Yellow
# Stop Sunshine
Stop-Process -Name sunshine -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

# Remove "test" device using the python script
if (Test-Path "$moonlightRoot\remove_test_device.py") {
    python "$moonlightRoot\remove_test_device.py"
}

# Start Sunshine again
Write-Host "--- Restarting Sunshine (Apollo) ---" -ForegroundColor Yellow
Start-Process "C:\Users\beelink\Applications\Files\Apollo\sunshine.exe" -WorkingDirectory "C:\Users\beelink\Applications\Files\Apollo\" -WindowStyle Hidden
Start-Sleep -Seconds 5

# Cleanup previous jobs
Get-Job | Remove-Job -Force

# Start tailing jobs
$moonJob1 = Start-Job -ScriptBlock { Get-Content $args[0] -Wait -Tail 0 | ForEach-Object { Write-Host "[MOON1] $_" -ForegroundColor White } } -ArgumentList $moonlightLog1
$moonJob2 = Start-Job -ScriptBlock { Get-Content $args[0] -Wait -Tail 0 | ForEach-Object { Write-Host "[MOON2] $_" -ForegroundColor Gray } } -ArgumentList $moonlightLog2
$sunJob = Start-Job -ScriptBlock { Get-Content $args[0] -Wait -Tail 0 | ForEach-Object { Write-Host "[SUN] $_" -ForegroundColor Yellow } } -ArgumentList $sunshineLog

# Start PPSSPP
Write-Host "--- Launching PPSSPP ---" -ForegroundColor Green
$proc = Start-Process -FilePath $ppssppExe -ArgumentList "`"$ebootPath`"" -WorkingDirectory "$moonlightRoot\PPSSPP_x64" -PassThru

# Monitor loop
while ($proc.HasExited -eq $false) {
    Receive-Job $moonJob1
    Receive-Job $moonJob2
    Receive-Job $sunJob
    Start-Sleep -Seconds 1
}

# Cleanup
Receive-Job $moonJob1; Receive-Job $moonJob2; Receive-Job $sunJob
Stop-Job $moonJob1; Remove-Job $moonJob1
Stop-Job $moonJob2; Remove-Job $moonJob2
Stop-Job $sunJob; Remove-Job $sunJob

Write-Host "PPSSPP exited." -ForegroundColor Red
