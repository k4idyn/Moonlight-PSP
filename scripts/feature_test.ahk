#NoEnv
SendMode Input
SetWorkingDir %A_ScriptDir%

; --- feature_test.ahk for deep codebase feature emulation ---
; This script is designed to emulate input for both the N3DS variant and PSP variants
; based on their precise UI abstractions and hardcoded logic models.

TestTarget = %1%

if (TestTarget = "N3DS") {
    ; N3DS Emulation Routine
    
    ; 1. Device Selection (Select "new" for custom IP)
    Send, {Down}{Enter}
    Sleep, 1000
    
    ; 2. IP Entry (swkbd typing)
    Send, 192.168.1.100{Enter}
    Sleep, 1000
    
    ; 3. Pairing
    ; Select pair from the menu
    Send, {Enter}
    Sleep, 2000
    ; Acknowledge PIN
    Send, {Enter}
    Sleep, 1000
    
    ; 4. App Selection
    Send, {Down}{Down}{Enter}
    Sleep, 1000
    
    ; 5. Settings Selection
    ; Assume settings menu is highlighted
    Send, {Down}{Down}{Enter}
    Sleep, 500
    ; Enter Bitrate
    Send, 4000{Enter}
    Sleep, 500
    
} else if (TestTarget = "PSP") {
    ; PSP Emulation Routine
    
    ; 1. IP Entry via custom D-Pad implementation
    ; We are at STATE_MAIN_MENU ("Empty" slot)
    Send, {Down}{Down}{Enter}
    Sleep, 500
    
    ; We are at STATE_ENTER_IP. Need to change "192.168." to "192.168.1.100"
    ; Enter '1'
    Send, {Up}
    Sleep, 100
    ; Move Right
    Send, {Right}
    Sleep, 100
    ; Append '.'
    Send, {Triangle}
    Sleep, 100
    ; Enter '1'
    Send, {Up}
    Sleep, 100
    ; Move Right
    Send, {Right}
    Sleep, 100
    ; Enter '0'
    Send, {Up}{Up}{Up}{Up}{Up}{Up}{Up}{Up}{Up}{Up}
    Sleep, 100
    ; Move Right
    Send, {Right}
    Sleep, 100
    ; Enter '0'
    Send, {Up}{Up}{Up}{Up}{Up}{Up}{Up}{Up}{Up}{Up}
    Sleep, 100
    ; Save configuration
    Send, {Enter} ; assuming enter maps to Cross in emu
    Sleep, 1000
    
    ; 2. Selecting device & Connecting
    ; Press cross on the now-saved IP (which automatically triggers hardcoded pairing & app selection & config)
    Send, {Enter}
    Sleep, 5000
}

ExitApp
