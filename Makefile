minecraftd: minecraftd.cpp
	g++ minecraftd.cpp -o minecraftd
clean: minecraftd
	rm -f minecraftd
install: minecraftd
	cp minecraftd /usr/local/bin/minecraftd
