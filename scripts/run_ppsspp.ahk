#NoEnv
SendMode Input
SetWorkingDir %A_ScriptDir%

maxAttempts := 1
attempt := 0
success := false

SetTitleMatchMode, 2

Loop {
    attempt++
    if (attempt > maxAttempts) {
        break
    }

    Process, Close, PPSSPPWindows64.exe

    Run, %A_ScriptDir%\PPSSPP_x64\PPSSPPWindows64.exe %A_ScriptDir%\psp_moonlight\EBOOT.PBP, %A_ScriptDir%\PPSSPP_x64, , ppid

    WinWait, ahk_pid %ppid%, , 10
    if (!ErrorLevel) {
        WinActivate, ahk_pid %ppid%
        WinWaitActive, ahk_pid %ppid%, , 5
        Sleep, 15000
        Loop 5 {
            WinActivate, ahk_pid %ppid%
            Send, {z down}
            Sleep, 200
            Send, {z up}
            Sleep, 2000
        }
    }

    SetTimer, CheckPopups, 100
    Process, WaitClose, %ppid%, 25
    if (ErrorLevel) {
        success := true
        SetTimer, CheckPopups, Off
        Process, Close, %ppid%
        break
    } else {
        SetTimer, CheckPopups, Off
        continue
    }
}
ExitApp

CheckPopups:
    WinWait, Network permission,, 1
    if (ErrorLevel = 0) {
        WinActivate
        Send, {Enter}
        return
    }
    WinWait, Crash detected,, 1
    if (ErrorLevel = 0) {
        WinActivate
        Send, {Enter}
        return
    }
    WinWait, Error,, 1
    if (ErrorLevel = 0) {
        WinActivate
        Send, {Enter}
        return
    }
    return
