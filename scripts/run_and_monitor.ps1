$moonlightRoot = "C:\Users\beelink\Desktop\moonlight3"
$pspMoonlightDir = "$moonlightRoot\psp_moonlight"
$ppssppExe = "$moonlightRoot\PPSSPP_x64\PPSSPPWindows64.exe"
$ebootPath = "$pspMoonlightDir\EBOOT.PBP"
$exceptionLog = "$moonlightRoot\PPSSPP_x64\memstick\exception.log"
$sunshineLog = "C:\Users\beelink\Applications\Files\Apollo\sunshine\logs\sunshine.log"
$moonlightLog = "$pspMoonlightDir\moonlight_debug.log"

Write-Host "--- Starting Absolute Perfection Monitoring ---" -ForegroundColor Cyan

# Clear old exception log
if (Test-Path $exceptionLog) { Remove-Item $exceptionLog }

# Start log monitors
$sunJob = Start-Job -ScriptBlock {
    param($log)
    if (Test-Path $log) {
        Get-Content $log -Wait -Tail 0 | ForEach-Object { Write-Host "[SUN] $_" }
    }
} -ArgumentList $sunshineLog

$moonJob = Start-Job -ScriptBlock {
    param($log)
    if (Test-Path $log) {
        Get-Content $log -Wait -Tail 0 | ForEach-Object { Write-Host "[MOON] $_" }
    }
} -ArgumentList $moonlightLog

# Start PPSSPP
$proc = Start-Process -FilePath $ppssppExe -ArgumentList $ebootPath -WorkingDirectory "$moonlightRoot\PPSSPP_x64" -PassThru

# Monitor loop
while (-not $proc.HasExited) {
    if (Test-Path $exceptionLog) {
        Write-Host "!!! EXCEPTION DETECTED !!!" -ForegroundColor Red
        Get-Content $exceptionLog | Write-Host -ForegroundColor Yellow
        break
    }
    
    # Receive job output
    Receive-Job $sunJob | Write-Host -ForegroundColor Gray
    Receive-Job $moonJob | Write-Host -ForegroundColor Green
    
    Start-Sleep -Seconds 1
}

Stop-Job $sunJob; Remove-Job $sunJob
Stop-Job $moonJob; Remove-Job $moonJob

if ($proc.HasExited) {
    Write-Host "PPSSPP finished with exit code $($proc.ExitCode)"
} else {
    Stop-Process -Id $proc.Id -Force
}
