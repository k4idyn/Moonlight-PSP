# Automated Absolute Perfection Loop for PSP Moonlight
# This script builds common-c and psp_moonlight, runs PPSSPP, and analyzes crashes.

$moonlightRoot = "C:\Users\beelink\Desktop\moonlight3"
$pspMoonlightDir = "$moonlightRoot\moonlight-psp-core"
$commonCDir = "$moonlightRoot\moonlight-common-c"
$ppssppDir = "$moonlightRoot\PPSSPP_x64"
$ppssppExe = "$ppssppDir\PPSSPPWindows64.exe"
$ebootPath = "$pspMoonlightDir\EBOOT.PBP"
$elfPath = "$pspMoonlightDir\moonlight-psp-core.elf"
$exceptionLog = "$ppssppDir\memstick\exception.log"
$addr2line = "$moonlightRoot\pspsdk\bin\psp-addr2line.exe"
$cmake = "$moonlightRoot\cmake-3.24.2-windows-x86_64\bin\cmake.exe"
$make = "$moonlightRoot\pspsdk\bin\make.exe"
$historyLog = "$moonlightRoot\perfection_history.log"

function Build-Project {
    Write-Host "--- Cleaning and Building Projects ---" -ForegroundColor Cyan
    
    # Ensure no stale build processes are running
    Stop-Process -Name make -Force -ErrorAction SilentlyContinue 2>$null
    
    # 1. Build common-c
    Write-Host "Building moonlight-common-c-master..."
    Push-Location $commonCDir
    if (-not (Test-Path "build-psp")) { New-Item -ItemType Directory "build-psp" | Out-Null }
    Push-Location "build-psp"
    # Use absolute path for cmake
    & $cmake .. -DCMAKE_TOOLCHAIN_FILE="$moonlightRoot/generic-psp-toolchain.cmake" -G "MinGW Makefiles" -DUSE_MBEDTLS=ON
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: CMake configuration failed for common-c" -ForegroundColor Red
        Pop-Location; Pop-Location; return $false
    }
    # Use -j1 to avoid make access violation on Windows
    & $make -j1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Failed to build common-c" -ForegroundColor Red
        Pop-Location; Pop-Location; return $false
    }
    Pop-Location; Pop-Location

    # 2. Build psp_moonlight
    Write-Host "Building moonlight-psp-core..."
    Push-Location $pspMoonlightDir
    $env:PATH = "C:\Users\beelink\Desktop\moonlight3\pspsdk\bin;" + $env:PATH
    & $make VERBOSE=1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Failed to build moonlight-psp-core" -ForegroundColor Red
        Pop-Location; return $false
    }
    Pop-Location
    
    return $true
}

function Analyze-Crash {
    if (Test-Path $exceptionLog) {
        Write-Host "!!! CRASH DETECTED !!!" -ForegroundColor Red
        $content = Get-Content $exceptionLog -Raw
        $epcMatch = [regex]::Match($content, "EPC:\s+(0x[0-9A-Fa-f]+)")
        if ($epcMatch.Success) {
            $epc = $epcMatch.Groups[1].Value
            Write-Host "Failure EPC: $epc" -ForegroundColor Yellow
            
            if (Test-Path $addr2line) {
                Write-Host "Translating EPC to Source Code..."
                $sourceInfo = & $addr2line -e $elfPath -f $epc
                Write-Host "Location: $sourceInfo" -ForegroundColor Green
                
                $timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"
                "$timestamp - EPC: $epc - $sourceInfo" | Out-File -FilePath $historyLog -Append
            }
        }
        # Keep the log for reference but move/rename it
        Move-Item $exceptionLog "$ppssppDir\memstick\exception_$(Get-Date -Format 'yyyyMMdd_HHmmss').log"

    } else {
        Write-Host "No crash log found. Clean run or unhandled exit." -ForegroundColor Green
    }
}

while ($true) {
    # 0. Cleanup and Stop Check
    if (Test-Path "$moonlightRoot\stop_loop.txt") {
        Write-Host "Stop file detected. Exiting loop." -ForegroundColor Cyan
        break
    }
    
    Stop-Process -Name PPSSPPWindows64 -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1

    # 1. Build
    $buildSuccess = Build-Project
    if (-not $buildSuccess) {
        Write-Host "Build failed. Retrying in 5 seconds..." -ForegroundColor Yellow
        Start-Sleep -Seconds 5
        continue
    }

    # 2. Run
    Write-Host "Launching PPSSPP..." -ForegroundColor Cyan
    Remove-Item $exceptionLog -ErrorAction SilentlyContinue
    $ppssppProcess = Start-Process -FilePath $ppssppExe -ArgumentList $ebootPath -WorkingDirectory $ppssppDir -PassThru
    
    Write-Host "Monitoring for 3 minutes (or until manual stop/crash)..."
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalMinutes -lt 3) {
        if ($ppssppProcess.HasExited) {
            Write-Host "PPSSPP exited unexpectedly." -ForegroundColor Yellow
            break
        }
        Start-Sleep -Seconds 5
        Write-Host -NoNewline "."
    }
    
    if ($timer.Elapsed.TotalMinutes -ge 3) {
        Write-Host "`nGoal Achieved: 3 minutes of stability!" -ForegroundColor Green
        Stop-Process -Name PPSSPPWindows64 -Force -ErrorAction SilentlyContinue
    }

    # 3. Analyze
    Analyze-Crash
    
    Write-Host "Loop cycle complete. Starting next iteration..." -ForegroundColor Gray
    Start-Sleep -Seconds 2
}
