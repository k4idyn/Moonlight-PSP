$moonlightRoot = "C:\Users\beelink\Desktop\moonlight3"
$ppssppExe = "$moonlightRoot\PPSSPPWindows64.exe"
$ebootPath = "$moonlightRoot\psp_moonlight\EBOOT.PBP"
$moonlightLog = "$moonlightRoot\memstick\moonlight.log"

Write-Host "--- Starting Absolute Perfection Monitoring ---" -ForegroundColor Cyan

# Start PPSSPP in the background
$proc = Start-Process -FilePath $ppssppExe -ArgumentList "`"$ebootPath`"" -PassThru

# Monitor the log file
Write-Host "Monitoring $moonlightLog..." -ForegroundColor Yellow
if (Test-Path $moonlightLog) { Remove-Item $moonlightLog }

$lastSize = 0
while (-not $proc.HasExited) {
    if (Test-Path $moonlightLog) {
        $stats = Get-Item $moonlightLog
        if ($stats.Length -gt $lastSize) {
            $raw = Get-Content $moonlightLog -Tail 10
            # Output only new lines (approximate)
            $raw | ForEach-Object { Write-Host "[MOON] $_" -ForegroundColor Green }
            $lastSize = $stats.Length
        }
    }
    Start-Sleep -Milliseconds 500
}

Write-Host "PPSSPP exited." -ForegroundColor Red
