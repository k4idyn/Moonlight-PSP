SetTitleMatchMode, 2
IfWinExist, PPSSPP
{
    WinActivate
    Sleep, 1000
    Send, {i down}  ; Triangle is mapped to 'i' by default in PPSSPP
    Sleep, 200
    Send, {i up}
}
