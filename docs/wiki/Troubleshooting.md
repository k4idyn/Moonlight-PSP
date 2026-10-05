# Troubleshooting

## Wi-Fi or connection errors

Reconnect the saved Wi-Fi profile and confirm the host is reachable on the same network. The PSP uses 2.4 GHz Wi-Fi. Crowded channels or a weak signal can interrupt streaming. Pair again when changing PSPs.

## Black screen or decoder errors

Keep `EBOOT.PBP` and `moonlight_me_helper.prx` from the same package together. Configure H.264 Baseline/CAVLC or Main/CABAC on the host. If reconnecting fails, leave Moonlight and launch it again.

## Audio gaps or delay

Quality and Balanced enable audio; Performance disables it. Confirm that the host application is producing sound. Bursty Wi-Fi traffic can cause gaps, repeated samples or delay.

## Fonts and mapping hints

Moonlight reads the PSP's built-in fonts from firmware. Use the mapping menu to assign actions and view analog direction hints. R+Up opens the HUD; Circle closes it.

## Video looks soft or blocky

Use Quality for native-resolution detail, Balanced for a middle point, or Performance for 30 fps. Fast-moving scenes and small desktop text can remain coarse at the PSP's low streaming bitrates.
