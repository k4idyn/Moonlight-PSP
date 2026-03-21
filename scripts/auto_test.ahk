#NoEnv
SendMode Input
SetWorkingDir %A_ScriptDir%
#SingleInstance Force

; ============================================================
; PSP Moonlight - Continuous Build + PPSSPP Test Loop
; Emulates real PSP-1000 hardware behavior in PPSSPP
; Runs until gs_init succeeds or max attempts reached
; ============================================================

PSPSDK       := "C:\Users\beelink\Desktop\moonlight3\pspsdk"
PPSSPP_EXE   := A_ScriptDir "\PPSSPP_x64\PPSSPPWindows64.exe"
EBOOT        := A_ScriptDir "\psp_moonlight\EBOOT.PBP"
LOG_FILE     := A_ScriptDir "\PPSSPP_x64\memstick\moonlight.log"
DBG_LOG      := A_ScriptDir "\PPSSPP_x64\memstick\moonlight_debug.log"
MAKEFILE_DIR := A_ScriptDir "\psp_moonlight"
maxAttempts  := 20
attempt      := 0
buildCount   := 0
PSPSDK_BIN   := "C:\Users\beelink\Desktop\moonlight3\pspsdk\bin"

; ---- Track test results across runs ----
Global lastError := "None"
Global successCount := 0

SetTitleMatchMode, 2
; Maximize screen for PPSSPP visibility
CoordMode, Mouse, Screen

; ---- Patch PPSSPP ini for PSP-1000 hardware emulation ----
PPSSPP_INI := A_ScriptDir "\PPSSPP_x64\memstick\PSP\SYSTEM\ppsspp.ini"
IniWrite, 0,   %PPSSPP_INI%, SystemParam, PSPModel        ; 0 = PSP-1000 (phat)
IniWrite, True,%PPSSPP_INI%, SystemParam, FirmwareVersion ; Use OFW compatible mode
; --- Make network stack identical to real PSP1000 ---
IniWrite, True, %PPSSPP_INI%, Network, EnableWlan
IniWrite, True, %PPSSPP_INI%, Network, AllowSocketsAPI
; --- Disable GPU HLE that can hide socket bugs ---
IniWrite, False, %PPSSPP_INI%, GPU, HardwareTransform
; --- Logging for debug ---
IniWrite, True, %PPSSPP_INI%, General, Enable Logging
IniWrite, True, %PPSSPP_INI%, General, FileLogging

Log("== PSP Moonlight Test Loop Started ==")
Log("Max attempts: " maxAttempts)

; ============================================================
; MAIN TEST LOOP
; ============================================================
Loop {
    attempt++
    if (attempt > maxAttempts) {
        MsgBox, 48, Max Attempts Reached, Failed after %maxAttempts% attempts.`nLast error: %lastError%
        break
    }

    Log("`n=== ATTEMPT " attempt "/" maxAttempts " ===")

    ; --- Kill any lingering instances ---
    KillAll()

    ; --- Clean old logs ---
    FileDelete, %LOG_FILE%
    FileDelete, %DBG_LOG%
    Sleep, 300

    ; --- Build (only on first attempt or after a code change) ---
    if (attempt == 1 || Mod(attempt, 3) == 0) {
        buildCount++
        Log("Building EBOOT (build #" buildCount ")...")
        RunWait, %ComSpec% /c "cd /d %MAKEFILE_DIR% && set PATH=%PATH%;%PSPSDK_BIN% && make -f Makefile.psp 2>&1",,Hide
        if (!FileExist(EBOOT)) {
            Log("BUILD FAILED - EBOOT not found")
            MsgBox, 48, Build Failed, EBOOT.PBP not produced on attempt %attempt%.
            continue
        }
        Log("Build OK - " EBOOT)
    }

    ; --- Launch PPSSPP ---
    Log("Launching PPSSPP...")
    Run, %PPSSPP_EXE% "%EBOOT%", %A_ScriptDir%\PPSSPP_x64, , ppid
    if (!ppid) {
        Log("ERROR: PPSSPP failed to launch")
        continue
    }

    ; --- Wait for PPSSPP window ---
    WinWait, ahk_pid %ppid%, , 8
    if (ErrorLevel) {
        Log("PPSSPP window never appeared")
        Process, Close, %ppid%
        continue
    }
    WinActivate, ahk_pid %ppid%
    Sleep, 2000

    ; --- Poll for up to 45s for key log entries ---
    startTime := A_TickCount
    testResult := "timeout"
    Loop {
        elapsed := (A_TickCount - startTime) / 1000
        if (elapsed > 45)
            break

        ; Check if PPSSPP died
        Process, Exist, %ppid%
        if (!ErrorLevel) {
            testResult := "ppsspp_died"
            break
        }

        ; Check logs
        FileRead, logContent, %LOG_FILE%
        FileRead, dbgContent, %DBG_LOG%

        ; SUCCESS: Made it past gs_init to connecting
        if (InStr(logContent, "Attempting serverinfo via") && InStr(logContent, "SUCCESS")) {
            testResult := "gs_init_ok"
            break
        }
        if (InStr(logContent, "connection established") || InStr(logContent, "Orbit Achieved")) {
            testResult := "streaming"
            break
        }

        ; KNOWN FAILURES - extract specifics
        if (InStr(dbgContent, "gs_init failed") || InStr(logContent, "gs_init failed")) {
            lastError := ExtractError(dbgContent . logContent)
            testResult := "gs_init_failed"
            break
        }
        if (InStr(dbgContent, "TCP connect failed")) {
            lastError := "TCP connect failed (firewall/IP?)"
            testResult := "tcp_failed"
            break
        }
        if (InStr(dbgContent, "DNS resolution failed")) {
            lastError := "DNS failed - server IP not reachable"
            testResult := "dns_failed"
            break
        }
        if (InStr(dbgContent, "TLS handshake FAILED")) {
            lastError := "TLS Handshake failed - SSL issue"
            testResult := "tls_failed"
            break
        }
        if (InStr(logContent, "WiFi connection TIMEOUT")) {
            lastError := "WiFi connection timed out"
            testResult := "wifi_timeout"
            break
        }

        Sleep, 500
    }

    Log("Result: " testResult " | Error: " lastError)
    ShowStatus(attempt, testResult, lastError, logContent, dbgContent)

    ; --- Close PPSSPP ---
    KillAll()
    Sleep, 1000

    ; --- Analyze and decide ---
    if (testResult == "gs_init_ok" || testResult == "streaming") {
        successCount++
        Log("SUCCESS! gs_init connected on attempt " attempt)
    }

    Log("Stopping after 1 attempt as requested.")
    break
}

Log("`n=== TEST LOOP COMPLETE ===")
ExitApp

; ============================================================
; FUNCTIONS
; ============================================================

KillAll() {
    Process, Close, PPSSPPWindows64.exe
    Sleep, 500
    ; Kill any zombie PPSSPP processes
    RunWait, %ComSpec% /c "taskkill /f /im PPSSPPWindows64.exe >nul 2>&1", , Hide
    Sleep, 300
}

Log(msg) {
    FormatTime, now,, yyyy-MM-dd HH:mm:ss
    FileAppend, %now% %msg%`n, %A_ScriptDir%\test_loop.log
}

ExtractError(text) {
    ; Try to pull the most useful error message from the debug log
    if (InStr(text, "gs_error="))  {
        RegExMatch(text, "gs_error=([^\n]+)", m)
        return m1
    }
    if (InStr(text, "[HTTP]")) {
        RegExMatch(text, "\[HTTP\] ([^\n]+)", m)
        return m1
    }
    return "Unknown error (check moonlight_debug.log)"
}

ShowStatus(attempt, result, err, logContent, dbgContent) {
    ; Build a short status report
    lines := []
    lines.Push("Attempt: " attempt)
    lines.Push("Result:  " result)
    lines.Push("Error:   " err)
    lines.Push("")

    ; Show last 5 lines of debug log
    if (StrLen(dbgContent) > 0) {
        lines.Push("--- moonlight_debug.log (last 400 chars) ---")
        dbgTail := SubStr(dbgContent, -400)
        lines.Push(dbgTail)
    }

    msg := ""
    For _, line in lines
        msg .= line "`n"

    ; Use a non-blocking tooltip instead of MsgBox so the loop can continue
    ToolTip, %msg%, 10, 10
    Sleep, 4000
    ToolTip ; clear
}