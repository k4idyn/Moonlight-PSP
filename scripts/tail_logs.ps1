$moonlightRoot = "C:\Users\beelink\Desktop\moonlight3"
$pspMoonlightDir = "$moonlightRoot\psp_moonlight"
$ppssppDir = "$moonlightRoot\PPSSPP_x64"
$sunshineLog = "C:\Users\beelink\Applications\Files\Apollo\sunshine\logs\sunshine.log"
$moonlightLog = "$ppssppDir\memstick\moonlight.log"
$moonlightDebugLog = "$pspMoonlightDir\moonlight_debug.log"
$exceptionLog = "$ppssppDir\memstick\exception.log"

Write-Host "--- Tailing Multiple Logs ---" -ForegroundColor Cyan

# Create files if they don't exist to avoid Get-Content errors
if (!(Test-Path $moonlightLog)) { New-Item -ItemType File -Path $moonlightLog -Force | Out-Null }
if (!(Test-Path $moonlightDebugLog)) { New-Item -ItemType File -Path $moonlightDebugLog -Force | Out-Null }

$jobs = @()

if (Test-Path $sunshineLog) {
    try {
        $jobs += Start-Job -ScriptBlock { param($log) Get-Content $log -Wait -Tail 0 | ForEach-Object { Write-Output "[SUN] $_" } } -ArgumentList $sunshineLog
    } catch { Write-Host "Failed to start SUN job: $_" }
}
if (Test-Path $moonlightLog) {
    try {
        $jobs += Start-Job -ScriptBlock { param($log) Get-Content $log -Wait -Tail 0 | ForEach-Object { Write-Output "[MOON] $_" } } -ArgumentList $moonlightLog
    } catch { Write-Host "Failed to start MOON job: $_" }
}
if (Test-Path $moonlightDebugLog) {
    try {
        $jobs += Start-Job -ScriptBlock { param($log) Get-Content $log -Wait -Tail 0 | ForEach-Object { Write-Output "[DEBUG] $_" } } -ArgumentList $moonlightDebugLog
    } catch { Write-Host "Failed to start DEBUG job: $_" }
}

while ($true) {
    foreach ($job in $jobs) {
        Receive-Job $job | ForEach-Object { Write-Host $_ }
    }
    if (Test-Path $exceptionLog) {
        Write-Host "!!! EXCEPTION DETECTED !!!" -ForegroundColor Red
        Get-Content $exceptionLog | Write-Host -ForegroundColor Yellow
        break
    }
    # Check if PPSSPP is still running
    if (!(Get-Process -Name PPSSPPWindows64 -ErrorAction SilentlyContinue)) {
        Write-Host "PPSSPP has exited." -ForegroundColor Yellow
        break
    }
    Start-Sleep -Milliseconds 500
}

foreach ($job in $jobs) { Stop-Job $job; Remove-Job $job }
