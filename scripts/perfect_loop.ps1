param (
    [switch]$SkipBuild,
    [switch]$RunOnly
)

$moonlightRoot = "C:\Users\beelink\Desktop\moonlight3"
$pspMoonlightDir = "$moonlightRoot\psp_moonlight"
$commonCDir = "$moonlightRoot\moonlight-common-c"
$ppssppDir = "$moonlightRoot\PPSSPP_x64"
$ppssppExe = "$ppssppDir\PPSSPPWindows64.exe"
$ebootPath = "$pspMoonlightDir\EBOOT.PBP"
$elfPath = "$pspMoonlightDir\PSP_Moonlight.elf"
$exceptionLog = "$ppssppDir\memstick\exception.log"
$sunshineLog = "C:\Users\beelink\Applications\Files\Apollo\sunshine\logs\sunshine.log"
$moonlightDebugLog = "$pspMoonlightDir\moonlight_debug.log"
$moonlightLog = "$ppssppDir\memstick\moonlight.log"
$addr2line = "$moonlightRoot\pspsdk\bin\psp-addr2line.exe"
$cmake = "$moonlightRoot\cmake-3.24.2-windows-x86_64\bin\cmake.exe"
$make = "$moonlightRoot\pspsdk\bin\make.exe"

function Build-Project {
    Write-Host "`n--- MISSION CALIBRATION: RECOMPILING BOTH CORES ---" -ForegroundColor Cyan
    
    # Ensure no stale processes are holding files
    Write-Host "Releasing file locks..." -ForegroundColor Gray
    # Stop any other running perfection loops (prevents log file lock conflicts).
    try {
        $thisPid = $PID
        $otherLoops = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
            $_.Name -eq 'powershell.exe' -and
            $_.ProcessId -ne $thisPid -and
            $_.CommandLine -and
            $_.CommandLine -like '*perfect_loop.ps1*'
        }
        foreach ($p in $otherLoops) {
            Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
        }
    } catch {
        # Non-fatal: best-effort only
    }

    Stop-Process -Name PPSSPPWindows64 -Force -ErrorAction SilentlyContinue
    Stop-Process -Name AutoHotkey -Force -ErrorAction SilentlyContinue
    Stop-Process -Name AutoHotkey64 -Force -ErrorAction SilentlyContinue
    Stop-Process -Name make -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 2

    function Write-TextFileWithRetry([string]$path, $content, [int]$retries = 6, [int]$delaySec = 2) {
        for ($i = 1; $i -le $retries; $i++) {
            try {
                # Ensure directory exists (should, but keep it robust)
                $dir = Split-Path -Parent $path
                if ($dir -and !(Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

                if (Test-Path $path) {
                    try { Remove-Item $path -Force -ErrorAction SilentlyContinue } catch { }
                }

                $content | Out-File $path -Encoding utf8 -Force
                return $true
            } catch {
                Write-Host "WARN: Failed writing log (attempt $i/$retries): $path" -ForegroundColor Yellow
                Start-Sleep -Seconds $delaySec
            }
        }
        return $false
    }
    
    # 1. Build moonlight-common-c
    Write-Host "Building moonlight-common-c..." -ForegroundColor Gray
    $commonBuildDir = "$commonCDir\build-psp"
    $commonBuildLog = "$moonlightRoot\build_common_c_runtime.log"
    if (Test-Path $commonBuildLog) { Remove-Item $commonBuildLog -Force }
    $commonLib = "$commonBuildDir\libmoonlight-common-c.a"

    $commonFatalReason = ""
    try {
        $commonOut = & $cmake --build $commonBuildDir --clean-first 2>&1
        $commonExit = $LASTEXITCODE
    } catch {
        $commonOut = @($_.Exception.Message)
        $commonExit = 1
    }

    if (!(Write-TextFileWithRetry -path $commonBuildLog -content $commonOut)) {
        Write-Host "ERROR: Could not write common-c build log (file locked?)." -ForegroundColor Red
        return $false
    }
    $commonTxt = ($commonOut -join "`n")

    $commonLibExists = Test-Path $commonLib
    if ($commonExit -ne 0) { $commonFatalReason += "exitCode=$commonExit " }
    if (!$commonLibExists) { $commonFatalReason += "missingArtifact " }

    # Extra diagnostics only; do not treat as hard-failure if exit/artifact are OK.
    $commonTxtHasError = ($commonTxt -match '(?im)(^|\n)\s*error:' -or $commonTxt -match 'undefined reference' -or $commonTxt -match '\bFAILED\b')
    if ($commonTxtHasError -and $commonExit -eq 0 -and $commonLibExists) {
        Write-Host "WARN: common-c log contains error-like text, but exit/artifact are OK." -ForegroundColor Yellow
    }

    if (($commonExit -ne 0) -or (!$commonLibExists)) {
        Write-Host "ERROR: moonlight-common-c build failed. Reason: $commonFatalReason See: $commonBuildLog" -ForegroundColor Red
        return $false
    }

    # 2. Build psp_moonlight
    Write-Host "Building psp_moonlight..." -ForegroundColor Gray
    Push-Location $pspMoonlightDir
    $env:PATH = "C:\Users\beelink\Desktop\moonlight3\pspsdk\bin;C:\Users\beelink\Desktop\moonlight3\pspsdk\libexec\gcc\psp\4.3.5;" + $env:PATH
    
    # Double check EBOOT.PBP is not locked
    if (Test-Path "EBOOT.PBP") {
        try {
            [IO.File]::OpenWrite((Join-Path (Get-Location) "EBOOT.PBP")).Close()
        } catch {
            Write-Host "CRITICAL ERROR: EBOOT.PBP is locked by another process!" -ForegroundColor Red
            Pop-Location; return $false
        }
    }

    $pspCleanLog = "$moonlightRoot\build_psp_clean_runtime.log"
    $pspBuildLog = "$moonlightRoot\build_psp_moonlight_runtime.log"
    $pspCleanOut = @()
    try {
        $pspCleanOut = & $make -f Makefile.psp clean 2>&1
    } catch {
        $pspCleanOut = @($_.Exception.Message)
    }
    if (!(Write-TextFileWithRetry -path $pspCleanLog -content $pspCleanOut)) {
        Write-Host "ERROR: Could not write psp_moonlight clean log (file locked?)." -ForegroundColor Red
        Pop-Location; return $false
    }

    $pspExit = 0
    try {
        $pspOut = & $make -f Makefile.psp 2>&1
        $pspExit = $LASTEXITCODE
    } catch {
        $pspOut = @($_.Exception.Message)
        $pspExit = 1
    }
    if (!(Write-TextFileWithRetry -path $pspBuildLog -content $pspOut)) {
        Write-Host "ERROR: Could not write psp_moonlight build log (file locked?)." -ForegroundColor Red
        Pop-Location; return $false
    }

    $pspLibOk = Test-Path $ebootPath
    if (($pspExit -ne 0) -or (!$pspLibOk)) {
        Write-Host "ERROR: psp_moonlight build failed. exitCode=$pspExit missingArtifact=$(!$pspLibOk) See: $pspBuildLog" -ForegroundColor Red
        Pop-Location; return $false
    }

    # Extra diagnostics only; do not treat as hard-failure if exit/artifact are OK.
    $pspTxt = Get-Content $pspBuildLog -Raw -ErrorAction SilentlyContinue
    $pspTxtHasError = ($pspTxt -match '(?im)(^|\n)\s*error:' -or $pspTxt -match 'undefined reference' -or $pspTxt -match '\bFAILED\b')
    if ($pspTxtHasError -and $pspExit -eq 0 -and $pspLibOk) {
        Write-Host "WARN: psp_moonlight log contains error-like text, but exit/artifact are OK." -ForegroundColor Yellow
    }
    Pop-Location
    
    Write-Host "SUCCESS: Both cores compiled with NO ERRORS." -ForegroundColor Green
    return $true
}

function Run-Tracked {
    Write-Host "`n--- INITIATING STREAM TRACKING (3 MINUTE TARGET) ---" -ForegroundColor Cyan
    
    if (-not (Test-Path $ebootPath)) {
        Write-Host "ERROR: EBOOT.PBP missing at $ebootPath" -ForegroundColor Red
        return $false
    }

    $achieved = $false

    # Clear old logs and signals
    if (Test-Path $exceptionLog) { Remove-Item $exceptionLog }
    $activeFlag = Join-Path $ppssppDir "memstick\streaming_active.flag"
    if (Test-Path $activeFlag) { Remove-Item $activeFlag }

    if (Test-Path $moonlightLog) { Remove-Item $moonlightLog -Force }
    if (Test-Path $moonlightDebugLog) { Remove-Item $moonlightDebugLog -Force }
    # Create empty files so Start-Job "Get-Content -Wait" starts streaming immediately
    New-Item -ItemType File -Path $moonlightLog -Force | Out-Null
    New-Item -ItemType File -Path $moonlightDebugLog -Force | Out-Null
    
    # Start Sunshine log tracking
    $sunJob = Start-Job -ScriptBlock {
        param($log)
        if (Test-Path $log) {
            Get-Content $log -Wait -Tail 0 | ForEach-Object { Write-Output "[SUN] $_" }
        }
    } -ArgumentList $sunshineLog

    # Start Moonlight log tracking (main)
    $moonMainJob = Start-Job -ScriptBlock {
        param($log)
        if (Test-Path $log) {
            Get-Content $log -Wait -Tail 0 | ForEach-Object { Write-Output "[MOON] $_" }
        }
    } -ArgumentList $moonlightLog

    # Start Moonlight debug log tracking
    $moonDebugJob = Start-Job -ScriptBlock {
        param($log)
        if (Test-Path $log) {
            Get-Content $log -Wait -Tail 0 | ForEach-Object { Write-Output "[DEBUG] $_" }
        }
    } -ArgumentList $moonlightDebugLog

    # Launch PPSSPP
    Write-Host "Launching PPSSPP..." -ForegroundColor Blue
    $proc = Start-Process -FilePath $ppssppExe -ArgumentList $ebootPath -WorkingDirectory $ppssppDir -PassThru

    $ppssppLaunchTime = Get-Date
    $awaitStreamActiveTimeoutSec = 300 # allow user time for PIN entry + app launch
    
    Write-Host "Waiting for active stream signal (video/audio sync)..." -ForegroundColor Magenta
    
    $startTime = $null
    $pollIntervalSec = 8  # poll/check cadence (>5s) while still streaming log lines via -Wait jobs
    $softErrorDetected = $false
    $softErrorReason = ""
    while ($true) {
        # Drain live log output from jobs (so we can react if anything fatal shows up)
        Receive-Job $sunJob -ErrorAction SilentlyContinue | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
        Receive-Job $moonMainJob -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host $_ -ForegroundColor Green
            if (-not $softErrorDetected) {
                if (($_ -match 'RTSP request timed out') -or
                    ($_ -match 'RTSP OPTIONS attempt') -or
                    ($_ -match 'Failed stage: .*RTSP')) {
                    $softErrorDetected = $true
                    $softErrorReason = $_
                }
            }
        }
        Receive-Job $moonDebugJob -ErrorAction SilentlyContinue | ForEach-Object {
            Write-Host $_ -ForegroundColor DarkGreen
            if (-not $softErrorDetected) {
                if (($_ -match 'RTSP request timed out') -or
                    ($_ -match 'RTSP OPTIONS attempt') -or
                    ($_ -match 'Failed stage: .*RTSP') -or
                    ($_ -match 'EXCEPTION DETECTED') -or
                    ($_ -match 'gs_pair failed')) {
                    $softErrorDetected = $true
                    $softErrorReason = $_
                }
            }
        }

        if ($softErrorDetected) {
            Write-Host "`n!!! ABSOLUTE PERFECTION INTERRUPTED: SOFT ERROR DETECTED !!!" -ForegroundColor Red
            Write-Host $softErrorReason -ForegroundColor Cyan
            $achieved = $false
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            break
        }

        if ($proc.HasExited) {
            Write-Host "PPSSPP terminated." -ForegroundColor Gray
            break
        }

        if ($startTime -eq $null) {
            $elapsedSinceLaunch = (Get-Date) - $ppssppLaunchTime
            if ($elapsedSinceLaunch.TotalSeconds -ge $awaitStreamActiveTimeoutSec) {
                Write-Host "Timeout: streaming_active.flag not created within $awaitStreamActiveTimeoutSec seconds. Restarting..." -ForegroundColor Yellow
                Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
                break
            }
        }
        
        if ($startTime -eq $null -and (Test-Path $activeFlag)) {
            $startTime = Get-Date
            Write-Host "`n[!] STREAM ACTIVE. STARTING 3-MINUTE PERFECTION COUNTDOWN." -ForegroundColor Cyan
            Remove-Item $activeFlag # Consume the flag
        }

        if (Test-Path $exceptionLog) {
            Write-Host "`n!!! ABSOLUTE PERFECTION INTERRUPTED: EXCEPTION DETECTED !!!" -ForegroundColor Red
            $content = Get-Content $exceptionLog -Raw
            Write-Host "--- LOG START ---" -ForegroundColor DarkGray
            Write-Host $content -ForegroundColor Cyan
            Write-Host "--- LOG END ---" -ForegroundColor DarkGray
            
            if ($content -match "EPC: (0x[0-9A-Fa-f]+)") {
                $epc = $matches[1]
                Write-Host "Analyzing failure point at $epc..." -ForegroundColor Yellow
                & $addr2line -e $elfPath -f $epc
            }
            
            $achieved = $false # any exception invalidates the stability requirement
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
            break
        }
        
        if ($startTime -ne $null) {
            $elapsed = (Get-Date) - $startTime
            if ($elapsed.TotalMinutes -ge 3) {
                Write-Host "`n[!] ABSOLUTE PERFECTION ACHIEVED: 3 MINUTES OF STABLE STREAMING" -ForegroundColor Green
                $achieved = $true
                Stop-Process -Id $proc.Id -Force
                break
            }
            Write-Progress -Activity "Streaming Perfection Tracker" -Status "Active Stream Time: $($elapsed.ToString('mm\:ss'))" -PercentComplete (($elapsed.TotalSeconds / 180) * 100)
        } else {
            Write-Progress -Activity "Streaming Perfection Tracker" -Status "Awaiting Connection..." -PercentComplete 0
        }
        
        Start-Sleep -Seconds $pollIntervalSec
    }
    
    # Cleanup jobs
    Stop-Job $sunJob; Remove-Job $sunJob
    Stop-Job $moonMainJob; Remove-Job $moonMainJob
    Stop-Job $moonDebugJob; Remove-Job $moonDebugJob

    return $achieved
}

# Main Loop Execution
while ($true) {
    $buildOk = $true
    if (-not ($SkipBuild -or $RunOnly)) {
        $buildOk = Build-Project
    }
    
    if ($buildOk) {
        $ok = Run-Tracked
        if ($ok -or $RunOnly) {
            if ($ok) {
                Write-Host "`n[OK] 3-minute stability requirement met. Exiting perfection loop." -ForegroundColor Green
            } else {
                Write-Host "`n[INFO] Run-Only mode finished." -ForegroundColor Gray
            }
            break
        } else {
            Write-Host "`n[RETRY] Streaming failed/crashed before 3 minutes." -ForegroundColor Yellow
            Read-Host "Press ENTER to rebuild and restart... (or Ctrl+C to stop)"
            Start-Sleep -Seconds 1
        }
    } else {
        Write-Host "BUILD FAILED. Please correct errors and press ENTER to retry." -ForegroundColor Red
        Start-Sleep -Seconds 2
    }
}
