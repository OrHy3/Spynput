CFLAGS = -I libevdev
LDFLAGS = -L libevdev/libevdev/.libs -levdev -lpthread

all: dynamic

dynamic: evdev
	mkdir build
	$(CC) -o build/logger logger.c $(CFLAGS) $(LDFLAGS)

static: evdev
	mkdir build
	$(CC) -static -o build/logger logger.c $(CFLAGS) $(LDFLAGS)

evdev:
	cd libevdev && ./autogen.sh
	cd libevdev && ./configure
	$(MAKE) -C libevdev

clean:
	rm -rf build

cleanall:
	$(MAKE) -C libevdev clean
	rm -rf build