Fork of the Tamagotchi Emulator 4 Pebble by StefanBauwens with following changes:
- added auto sync of the time on start and all 2 hours if drift is above 30 seconds
- autosave all 5 minutes
- save states are stored on watch (fallback via phone)
- added vibration when tamagotchi needs something
- support for tama-p1.bin to have it on the watch, no need to load rom from watch on start anymore
- added time, battery status and date on the frame around the tamagotchi screen (updated all 30 seconds)
- some small fix to avoid auto close when idle

- to do: upload script to modify tama-p1.bin that are found all around the internet to be compatible

- With binary on watch basically all phone dependency has been removed. App runs without the need of the phone 

- Note: bin file needs to be put into resources/data folder, not provided here in this repo!
