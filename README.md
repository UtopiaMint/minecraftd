# minecraftd
The Minecraft daemon wrapper, written in C++

Clone it.

```git clone https://github.com/lkp111138/minecraftd```

Compile it.

```make minecraftd```

Run it.

```sudo ./minecraftd -d```

Please also have the directories ```/etc/minecraftd```, ```/etc/minecraftd/data``` and user minecraft with uid and gid both 25565 created and ```spigot-1.11.2.jar``` available in ```/etc/minecraftd/data``` before running.

```minecraftd``` is in beta so bugs may exist, and certain features are unavailable, new functions will be rolled out soon!
Features:
- Daemon mode
- Auto restart (5 seconds apart, max 5 times, within 60 secs. ```minecraftd``` will terminate itself after the 5th death of the server within a minute)
- Custonizable memory allocation (default 30% ~ 70% of total RAM)
- Customizable jarfile path (default to ```/etc/minecraftd/data/spigot-1.11.2.jar```)

Planned features: 
- Data path (Currently hardcoded to ```/etc/minecraftd/data/```)
- Run as different uid/gid
- ...And more!
