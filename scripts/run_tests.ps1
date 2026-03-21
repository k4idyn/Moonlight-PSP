Write-Host "Starting feature testing for Moonlight platforms..."

# Run N3DS test stub
Write-Host "Testing N3DS Moonlight features..."
if (Test-Path ".\feature_test.ahk") {
    Start-Process "AutoHotkey.exe" -ArgumentList ".\feature_test.ahk", "N3DS" -Wait
}
Write-Host "N3DS Test complete. Log generated."

# Run PSP (PPSSPP)
Write-Host "Testing PSP Moonlight features..."
$PPSSPP_Path = ".\PPSSPP_x64\PPSSPPWindows64.exe"
$PSP_Moonlight = ".\psp_moonlight\EBOOT.PBP"

if (Test-Path $PPSSPP_Path) {
    Start-Process -FilePath $PPSSPP_Path -ArgumentList $PSP_Moonlight
    Start-Sleep -Seconds 5
    
    # Run AHK
    if (Test-Path ".\feature_test.ahk") {
        Write-Host "Running AHK interactions for PSP..."
        Start-Process "AutoHotkey.exe" -ArgumentList ".\feature_test.ahk", "PSP" -Wait
    }
    
    Stop-Process -Name "PPSSPPWindows64" -ErrorAction SilentlyContinue
    Write-Host "PSP Test complete. Log generated."
} else {
    Write-Host "PPSSPP not found."
}

Write-Host "All feature testing completed."
