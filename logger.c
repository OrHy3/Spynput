#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <libevdev/libevdev.h>

enum {
	DELETION_MARK = 0x01,
	NEXT_IS_LOCKED = 0x02,
	APPEND_MARK = 0x04,
	APPEND_CONFIRM = 0x08,
	ALL = 0xff
};

struct device_entry {
	struct libevdev *dev;
	int fd;
	unsigned flags;
	struct device_entry *prev;
	struct device_entry *next;
};

// Linked list's first entry
struct device_entry *root_entry = NULL;

// Device manager thread
pthread_t manager_t;

// device_entry constructor
struct device_entry* new_device_entry() {
	struct device_entry *dev = malloc(sizeof(struct device_entry));
	if (dev == NULL)
		return NULL;
	dev->dev = NULL;
	dev->fd = -1;
	dev->flags = 0;
	dev->prev = NULL;
	dev->next = NULL;
	return dev;
}

// device_entry destructor + removal from the linked list
void free_device_entry(struct device_entry *dev) {
	if (dev == NULL)
		return;

	if (dev == root_entry)
		root_entry = dev->next;

	if (dev->next == dev)
		root_entry = NULL;

	if (dev->next)
		dev->next->prev = dev->prev;
	if (dev->prev) {
		dev->prev->next = dev->next;
		dev->prev->flags &= ~NEXT_IS_LOCKED;
	}

	if (dev->dev)
		libevdev_free(dev->dev);
	if (dev->fd >= 0)
		close(dev->fd);
	free(dev);
}

// Function to filter device types
int check_capabilities(struct libevdev *dev) {
	
	if (libevdev_has_event_type(dev, EV_KEY)) {

		// Likely a keyboard
		if (libevdev_has_event_code(dev, EV_KEY, KEY_A))
			return 1;

		// Likely a mouse
		if (libevdev_has_event_code(dev, EV_KEY, BTN_LEFT))
			return 1;

	}

	// Likely a controller
	if (libevdev_has_event_type(dev, EV_ABS) && libevdev_has_event_code(dev, EV_ABS, ABS_X))
		return 1;

	return 0;

}

// Creates and add a new entry to the linked list
int add_new_entry(char *device_name) {

	char path[1024] = "/dev/input/";

	int fd = open(strcat(path, device_name), O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return 1;

	struct libevdev *new_device;
	int status = libevdev_new_from_fd(fd, &new_device);
	if (status < 0) {
		close(fd);
		return 2; 
	}

	if (check_capabilities(new_device)) {

		struct device_entry *new_entry = new_device_entry();
		if (new_entry == NULL) {
			close(fd);
			libevdev_free(new_device);

			return 4;
		}
		
		new_entry->dev = new_device;
		new_entry->fd = fd;
		
		if (root_entry == NULL) {
			new_entry->prev = new_entry;
			new_entry->next = new_entry;
			root_entry = new_entry;
		}
		else {
			
			// Wait for consent
			root_entry->prev->flags |= APPEND_MARK;
			while (!(root_entry->prev->flags & APPEND_CONFIRM));

			new_entry->prev = root_entry->prev;
			new_entry->next = root_entry;

			new_entry->prev->next = new_entry;
			root_entry->prev = new_entry;

			new_entry->prev->flags &= ~(APPEND_MARK | APPEND_CONFIRM);

		}

		return 0;
	
	}

	close(fd);
	libevdev_free(new_device);
	return 3;

}

// Function to retrieve and store the entries
int store_entries() {

	DIR *dev_input = opendir("/dev/input/");

	if (dev_input == NULL)
		return 3;

	struct dirent *entry;

	// Process any found file
	while ((entry = readdir(dev_input)) != NULL)

		// Must be an event file
		if (strncmp(entry->d_name, "event", 5) == 0) {
			if (root_entry)
				root_entry->prev->flags |= ALL;
			int status = add_new_entry(entry->d_name);
			if (status != 0 && status != 3)
				return status;
			if (root_entry)
				root_entry->prev->prev->flags = 0;
		}

	return 0;

}

// Flag for the manager's thread
unsigned char quitting = 0;
// Directory watcher
int dir_watcher;

// Signal handler
void handler(int) {
	fputs("\nExiting...\n", stderr);
	quitting = 1;
	pthread_join(manager_t, NULL);
	while (root_entry)
		free_device_entry(root_entry->prev);
	close(dir_watcher);
	exit(0);
}

// Error logging
void process_error(int err) {
	switch (err) {

		case 1:
			fputs("Could not open event file\n", stderr);
			exit(1);

		case 2:
			fputs("Could not initialize event device\n", stderr);
			exit(1);

		case 3:
			fputs("Could not read /dev/input\n", stderr);
			exit(1);

		case 4:
			fputs("Could not allocate memory\n", stderr);
			exit(1);

	}
}

// Manager thread function
void* manage_devices(void*) {

	struct device_entry *current_entry = root_entry;
	while (!quitting) {

		// Prevent segfaults on a empty list
		struct device_entry *next_entry = NULL;
		if (current_entry)
			next_entry = current_entry->next;

		// Remove disconnected devices
		if (current_entry && current_entry->flags & DELETION_MARK)
			free_device_entry(current_entry);

		// Make sure to be on the list
		if (next_entry)
			current_entry = next_entry;
		else
			current_entry = root_entry;

		// Read device creation events
		char buffer[1024];
		int length = read(dir_watcher, buffer, sizeof(buffer));

		// Skip if no event
		if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			continue;

		// Exit if read() returned with error or inotify closed
		if (length <= 0)
			exit(1);

		// Loop through inotify events
		for (int i = 0; i < length;) {

			struct inotify_event *event = (struct inotify_event*)(buffer + i);

			// Add new event devices
			if (event->len && event->mask & IN_CREATE && strncmp(event->name, "event", 5) == 0)
				add_new_entry(event->name);

			i += sizeof(struct inotify_event) + event->len;

		}

	}
}

int main() {

	// Prevent stdout buffering
	setbuf(stdout, NULL);

	// SIGINT handler
	if (signal(SIGINT, handler) == SIG_ERR) {
		fputs("Could not set signal handler\n", stderr);
		return 1;
	}

	// Setup inotify to listen for new devices
	dir_watcher = inotify_init1(IN_NONBLOCK);
	if (dir_watcher < 0 || inotify_add_watch(dir_watcher, "/dev/input", IN_CREATE) == -1) {
		fputs("Could not initialize inotify\n", stderr);
		return 1;
	}

	// Initial list setup
	process_error(store_entries());
	
	// Device manager thread
	if (pthread_create(&manager_t, NULL, manage_devices, (void*)0) != 0) {
		fputs("Could not create new thread\n", stderr);
		return 1;
	}

	struct device_entry *current_entry = root_entry;

	while (1) {

		// Check wether the list is available yet
		if (current_entry == NULL) {
			current_entry = root_entry;
			continue;
		}

		struct input_event ev;
		unsigned char skip = 0;

		switch (libevdev_next_event(current_entry->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) {

			// Device requires syncing
			case LIBEVDEV_READ_STATUS_SYNC:
				libevdev_next_event(current_entry->dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
				break;

			// Event read
			case LIBEVDEV_READ_STATUS_SUCCESS:
				break;

			// No event
			case -EAGAIN:
				skip = 1;
				break;

			// Device disconnected
			case -ENODEV:

				// Prevent segfault on empty list
				if (current_entry->next == current_entry) {
					current_entry = NULL;
					root_entry->flags |= DELETION_MARK;
					root_entry = NULL;
				}
				// Prevent race conditions on non-empty list
				else {
					current_entry->prev->flags |= NEXT_IS_LOCKED;
					current_entry->flags |= DELETION_MARK;
				}
				skip = 1;
				break;

			default:

				// Panic if it's a libevdev internal error
				fputs("There was an unexpected behavior\n", stderr);
				exit(1);

		}

		// Nothing to do
		if (current_entry == NULL)
			continue;

		// Print relevant events only
		if (ev.type != EV_SYN && !skip)
			printf("%s %s %d\n",
				libevdev_event_type_get_name(ev.type),
				libevdev_event_code_get_name(ev.type, ev.code),
				ev.value
			);

		// If no conflicts with the manager thread, go to next entry
		if (!(current_entry->flags & (NEXT_IS_LOCKED | APPEND_CONFIRM))) {

			current_entry = current_entry->next;

			// Allow new entry creation
			if (current_entry->prev->flags & APPEND_MARK)
				current_entry->prev->flags |= APPEND_CONFIRM;

		// Allow new entry creation
		} else if (current_entry->flags & APPEND_MARK)
			current_entry->flags |= APPEND_CONFIRM;

	}

	return 0;

}