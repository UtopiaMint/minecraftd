# minecraftd
The Minecraft daemon wrapper, written in C++

Clone it.

```git clone https://github.com/lkp111138/minecraftd```

Create necessary user and group and the data directory

```groupadd -g 25565 minecraft
useradd -u 25565 -g 25565 -d /etc/minecraftd -m -k /dev/null -s /bin/false minecraft
sudo -u minecraft mkdir -p /etc/minecraftd/data```

Compile it.

```make minecraftd```

Run it.

```sudo ./minecraftd -d```

```minecraftd``` is in beta so bugs may exist, and certain features are unavailable, new functions will be rolled out soon!
Features:
- Daemon mode
- Auto restart (5 seconds apart, max 5 times, within 60 secs. ```minecraftd``` will terminate itself after the 5th death of the server within a minute)
- Custonizable memory allocation (default 30% ~ 70% of total RAM, use ```minheap``` and ```maxheap``` directive in config.txt)
- Customizable jarfile path (default to ```/etc/minecraftd/data/spigot-1.11.2.jar```, use the ```jarfile``` directive in config.txt)

Planned features: 
- Data path (Currently hardcoded to ```/etc/minecraftd/data/```)
- Run as different uid/gid
- ...And more!
