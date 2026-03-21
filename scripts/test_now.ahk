#NoEnv
SetTitleMatchMode, 2
Process, Close, PPSSPPWindows64.exe
FileDelete, C:\Users\beelink\Desktop\moonlight3\PPSSPP_x64\memstick\moonlight.log
FileDelete, C:\Users\beelink\Desktop\moonlight3\PPSSPP_x64\memstick\moonlight_debug.log

Run, C:\Users\beelink\Desktop\moonlight3\PPSSPP_x64\PPSSPPWindows64.exe C:\Users\beelink\Desktop\moonlight3\psp_moonlight\EBOOT.PBP

WinWait, PPSSPP,, 10
if (ErrorLevel) {
    ExitApp
}
WinActivate, PPSSPP

; Wait for EBOOT to load and WiFi warmup (10s total)
Sleep, 10000

; Click X to connect to WiFi
Send, {x down}
Sleep, 100
Send, {x up}

; Wait for IP address and server search (12s)
Sleep, 12000

; Click X to select the first server (10.0.0.73)
Send, {x down}
Sleep, 100
Send, {x up}

; Wait for Stream to establish and run for a few seconds
Sleep, 20000

Process, Close, PPSSPPWindows64.exe
ExitApp
