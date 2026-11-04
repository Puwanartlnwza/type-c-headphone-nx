# usbaudio sysmodule (v0.1, untested)

Records the Switch's final audio output (`audrec:u`) and plays it on Apple USB-C EarPods via `usb:hs`.

## Install
(The package contains `exefs.nsp` + `flags/boot2.flag`; delete any old `exefs/` folder in the same title folder first.)
1. Download the `usbaudio-sysmodule` artifact from GitHub Actions and copy its `atmosphere/` folder to the SD card root.
2. Reboot. Plug in the EarPods. Log: `sd:/config/usbaudio/log.txt`.
3. Optional `sd:/config/usbaudio/config.ini`: `gain=40` (percent, 0-100).

## If something goes wrong
- Boot loop / crash: delete `sd:/atmosphere/contents/4200000000005B1A/flags/boot2.flag` (or the whole `4200000000005B1A` folder).
- Lower `gain` before putting the earphones in.
