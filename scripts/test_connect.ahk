#NoEnv
Run, "C:\Users\beelink\Desktop\moonlight3\PPSSPP_x64\PPSSPPWindows64.exe" "C:\Users\beelink\Desktop\moonlight3\PPSSPP_x64\memstick\PSP\GAME\PSP_Moonlight\EBOOT.PBP"
WinWait, PPSSPP
WinActivate, PPSSPP
Sleep, 5000
Send, x
Sleep, 1000
Send, x
; Longer sleep to allow time to enter PIN in Sunshine and for the stream to establish
Sleep, 60000
Process, Close, PPSSPPWindows64.exe
ExitApp
