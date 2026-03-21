$moonlightRoot = "C:\Users\beelink\Desktop\moonlight3"
$ppssppExe = "$moonlightRoot\PPSSPP_x64\PPSSPPWindows64.exe"
$ebootPath = "$moonlightRoot\psp_moonlight\EBOOT.PBP"
$moonlightLog = "$moonlightRoot\psp_moonlight\moonlight_debug.log"
$sunshineLog = "C:\Users\beelink\Applications\Files\Apollo\config\sunshine.log"

Write-Host "--- Starting Absolute Perfection Monitoring v2 ---" -ForegroundColor Cyan
Write-Host "Monitoring Moonlight: $moonlightLog" -ForegroundColor Green
Write-Host "Monitoring Sunshine: $sunshineLog" -ForegroundColor Yellow

# Clear old logs if they exist
if (Test-Path $moonlightLog) { Remove-Item $moonlightLog }

# Start PPSSPP
$proc = Start-Process -FilePath $ppssppExe -ArgumentList "`"$ebootPath`"" -WorkingDirectory "$moonlightRoot\PPSSPP_x64" -PassThru

$lastSizeMoon = 0
$lastSizeSun = 0

if (Test-Path $sunshineLog) {
    $lastSizeSun = (Get-Item $sunshineLog).Length
}

while (-not $proc.HasExited) {
    # Monitor Moonlight Log
    if (Test-Path $moonlightLog) {
        $stats = Get-Item $moonlightLog
        if ($stats.Length -gt $lastSizeMoon) {
            Get-Content $moonlightLog | Select-Object -Skip (($lastSizeMoon / 100) -as [int]) | ForEach-Object { 
                if ($_ -ne $null) { Write-Host "[MOON] $_" -ForegroundColor Green }
            }
            # Simple way to track progress for now
            $lastSizeMoon = $stats.Length
        }
    }

    # Monitor Sunshine Log
    if (Test-Path $sunshineLog) {
        $stats = Get-Item $sunshineLog
        if ($stats.Length -gt $lastSizeSun) {
             # Only show new content
             Get-Content $sunshineLog | Select-Object -Last 5 | ForEach-Object {
                Write-Host "[SUN] $_" -ForegroundColor Yellow
             }
             $lastSizeSun = $stats.Length
        }
    }

    Start-Sleep -Seconds 1
}

Write-Host "PPSSPP exited." -ForegroundColor Red
