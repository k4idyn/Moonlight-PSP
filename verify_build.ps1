# =============================================================================
# PSP Moonlight - PPSSPP Automated Verification Script (PowerShell)
# Launches PPSSPP, sends keystrokes to navigate the menu, monitors logs.
# =============================================================================

$ErrorActionPreference = "Continue"

# --- Configuration ---
$PpssppExe    = "C:\Users\beelink\Desktop\moonlight3\PPSSPP_x64\PPSSPPWindows64.exe"
$MemstickDir  = "C:\Users\beelink\Desktop\moonlight3\PPSSPP_x64\memstick"
$EbootSrc     = "C:\Users\beelink\Desktop\moonlight-psp-core\EBOOT.PBP"
$EbootDst     = "$MemstickDir\PSP\GAME\PSP_Moonlight\EBOOT.PBP"
$MoonlightLog = "$MemstickDir\moonlight.log"
$DebugLog     = "$MemstickDir\PSP\GAME\PSP_Moonlight\moonlight_debug.log"
$ResultsFile  = "C:\Users\beelink\Desktop\moonlight-psp-core\test_results.txt"

# --- Helper: SendKeys via WScript.Shell ---
Add-Type -AssemblyName System.Windows.Forms

function Send-PspKey {
    param([string]$Key, [int]$DelayMs = 300)
    $wsh = New-Object -ComObject WScript.Shell
    Write-Host "Sending Key: $Key (Wait: $DelayMs ms)"
    # Activate PPSSPP and wait a bit for focus
    if ($wsh.AppActivate("PPSSPP")) {
        Start-Sleep -Milliseconds 200
        [System.Windows.Forms.SendKeys]::SendWait($Key)
        Start-Sleep -Milliseconds $DelayMs
    } else {
        Write-Warning "Could not activate PPSSPP window!"
    }
}

function Wait-LogPattern {
    param([string]$Pattern, [int]$TimeoutSec = 30, [string]$LogFile = $MoonlightLog)
    $deadline = [DateTime]::Now.AddSeconds($TimeoutSec)
    while ([DateTime]::Now -lt $deadline) {
        foreach ($lf in @($MoonlightLog, $DebugLog)) {
            if (Test-Path $lf) {
                $content = Get-Content $lf -Raw -ErrorAction SilentlyContinue
                if ($content -and $content -match [regex]::Escape($Pattern)) {
                    return $true
                }
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

# --- Results tracking ---
$results = @()
function Log-Result {
    param([string]$Test, [string]$Status, [string]$Detail = "")
    $ts = Get-Date -Format "HH:mm:ss"
    $line = "[$ts] $Test : $Status"
    if ($Detail) { $line += " ($Detail)" }
    Write-Host $line
    $script:results += $line
}

# =============================================================================
# MAIN TEST SEQUENCE
# =============================================================================

Write-Host "============================================"
Write-Host "  PSP Moonlight PPSSPP Verification Test"
Write-Host "  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Write-Host "============================================"
Write-Host ""

# 0. Pre-flight
Stop-Process -Name PPSSPPWindows64 -Force -ErrorAction SilentlyContinue
Start-Sleep 1
Remove-Item $MoonlightLog -Force -ErrorAction SilentlyContinue
Remove-Item $DebugLog -Force -ErrorAction SilentlyContinue

# 1. Deploy EBOOT
Write-Host "[1] Deploying EBOOT.PBP..."
if (-not (Test-Path $EbootSrc)) {
    Log-Result "EBOOT Deploy" "FAIL" "Source not found: $EbootSrc"
    $results | Out-File $ResultsFile -Encoding utf8
    exit 1
}
New-Item -ItemType Directory -Force (Split-Path $EbootDst) | Out-Null
Copy-Item $EbootSrc $EbootDst -Force
$sz = (Get-Item $EbootDst).Length
Log-Result "EBOOT Deploy" "PASS" "$sz bytes"

# 2. Launch PPSSPP
Write-Host "[2] Launching PPSSPP..."
$proc = Start-Process $PpssppExe -ArgumentList "`"$EbootDst`"" -WorkingDirectory (Split-Path $PpssppExe) -PassThru
Start-Sleep 3

if ($proc.HasExited) {
    Log-Result "PPSSPP Launch" "FAIL" "Process exited immediately"
    $results | Out-File $ResultsFile -Encoding utf8
    exit 1
}
Log-Result "PPSSPP Launch" "PASS" "PID=$($proc.Id)"

# 3. Wait for boot
Write-Host "[3] Waiting 12s for app boot..."
Start-Sleep 12

# 4. Handle Wi-Fi Selector
Write-Host "[4] Waiting for Wi-Fi Selector..."
if (Wait-LogPattern "Entering Wi-Fi Selector Loop..." 30) {
    Send-PspKey "z" 1000
    Log-Result "Wi-Fi Select" "PASS"
} else {
    Log-Result "Wi-Fi Select" "FAIL" "Timeout waiting for Wi-Fi selector"
}

# 5. Wait for "ALL SYSTEMS GO" (initialization complete)
Write-Host "[5] Waiting for Module Initialization..."
if (Wait-LogPattern "All modules initialized successfully." 30) {
    Log-Result "Module Init" "PASS"
} else {
    Log-Result "Module Init" "FAIL" "Timeout waiting for module initialization"
}

# 6. Check for host detection or main loop
Write-Host "[6] Waiting for Main Loop..."
if (Wait-LogPattern "Entering main loop..." 15) {
    Log-Result "Main Loop" "PASS"
} else {
    Log-Result "Main Loop" "FAIL" "Timeout waiting for main loop"
}

# 7. Check for PIN generation (Pairing)
Write-Host "[7] Monitoring for Pairing PIN..."
if (Wait-LogPattern "Pairing required. PIN:" 30) {
    # Extract PIN from log
    $pattern = "Pairing required. PIN: (\d{4})"
    foreach ($lf in @($MoonlightLog, $DebugLog)) {
        if (Test-Path $lf) {
            $content = Get-Content $lf -Raw -ErrorAction SilentlyContinue
            if ($content -and $content -match $pattern) {
                $pin = $Matches[1]
                Log-Result "Pairing PIN" "GENERATED" "PIN=$pin"
                Write-Host "************************"
                Write-Host "*  PAIRING PIN: $pin  *"
                Write-Host "************************"
                break
            }
        }
    }
} else {
    Log-Result "Pairing PIN" "NOT FOUND" "Either already paired or failed to reach pairing step"
}

# --- Cleanup ---
Write-Host ""
Write-Host "[Cleanup] Stopping PPSSPP..."
Stop-Process -Name PPSSPPWindows64 -Force -ErrorAction SilentlyContinue
Start-Sleep 1

# --- Write results ---
Write-Host ""
Write-Host "============================================"
Write-Host "  TEST RESULTS SUMMARY"
Write-Host "============================================"
$results | ForEach-Object { Write-Host $_ }

# Save results
$results | Out-File $ResultsFile -Encoding utf8

# Append raw logs for comparison
if (Test-Path $DebugLog) {
    "`n=== MOONLIGHT_DEBUG.LOG ===" | Out-File $ResultsFile -Append -Encoding utf8
    Get-Content $DebugLog -ErrorAction SilentlyContinue | Out-File $ResultsFile -Append -Encoding utf8
}

Write-Host ""
Write-Host "Results saved to: $ResultsFile"
Write-Host "Done."
