#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <pthread.h>
#include <linux/input.h>
#include <libevdev/libevdev.h>
#include <fcntl.h>
#include <errno.h>

#include "config.h"

#define LENGTH(X) sizeof X / sizeof X[0]
#define INPUT_VAL_PRESS 1
#define INPUT_VAL_RELEASE 0
#define INPUT_VAL_REPEAT 2

static _Atomic(unsigned int) mod_state = 0;
static _Atomic(key_code) last_press = 0;
static _Atomic(pid_t) pid;


void print_usage(const char *program) {
	fprintf(stdout,
	        "\n"
	        "usage: %s [options]\n"
	        "\n"
	        "Signal hkd based on key events"
	        "\n"
	        "options:\n"
	        " -h        show this message and exit\n",
	        program);
}


/**
 * Write an event to stdout
 */
void write_event(const struct input_event *event) {
	printf("%d\n", event->code);
}


/**
 * @return the modifier mask of the given key
 */
unsigned int get_mod_mask(key_code key) {
	int i = 0, j;
	unsigned int bits;
	for (bits = 1 << (LENGTH(mods) - 1); bits; bits >>= 1) {
		for (j = 0; j < LENGTH(mods[i]); j++) {
			if (mods[i][j] == key) return bits;
		}
		i++;
	}
	return 0;
}


/**
 * Attempt to run associated command
 *
 * @return whether the key combination is a hotkey
 */
int try_hotkey(key_code key) {
	int i;
	union sigval msg;
	for (i = 0; i < LENGTH(bindings); i++) {
		if (bindings[i].key == key
		 && bindings[i].mod_mask == mod_state) {
			msg.sival_int = i;
			sigqueue(pid, SIGUSR1, msg);
			return 1;
		}
	}
	return 0;
}


void handle_event(struct input_event input) {
	if (input.type == EV_MSC && input.code == MSC_SCAN) return;

	/* forward anything that is not a key event, including SYNs */
	if (input.type != EV_KEY) {
		/* write_event(&input); */
		return;
	}

	/* handle keys */
	unsigned int mod_mask;
	switch (input.value) {
		case INPUT_VAL_PRESS:
			last_press = input.code;
			if ((mod_mask = get_mod_mask(input.code))
			 || !try_hotkey(input.code)) {
				mod_state |= mod_mask;
				/* write_event(&input); */
			}
			return;
		case INPUT_VAL_RELEASE:
			if ((mod_mask = get_mod_mask(input.code))) {
				mod_state ^= mod_mask;
				if (last_press == input.code) try_hotkey(input.code);
			}
			/* write_event(&input); */
			return;
		case INPUT_VAL_REPEAT:
			/* linux console, X, wayland handles repeat */
			return;
		default:
			fprintf(stderr, "unexpected .value=%d .code=%d, doing nothing",
			        input.value,
			        input.code);
			return;
	}
	return;
}


void *handle_device(void *path) {
	/* open device */
	struct libevdev *dev = NULL;
	int fd = open((char*)path, O_RDONLY|O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "Error: failed to open device: %s\n", (char*)path);
		exit(EXIT_FAILURE);
	}
	int rc = libevdev_new_from_fd(fd, &dev);
	if (rc < 0) {
		fprintf(stderr, "Error: failed to init libevdev (%s)\n", strerror(-rc));
		exit(EXIT_FAILURE);
	}

	/* process events */
	do {
		struct input_event input;
		rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &input);
		if (rc == 0) handle_event(input);
	} while (rc == 1 || rc == 0 || rc == -EAGAIN);

	return NULL;
}


int main(int argc, char *argv[]) {
	/* ensure program is running as root */
	if (geteuid()) {
		fprintf(stderr, "%s: must be run as root user\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	/* find hkd pid */
	FILE *cache = popen("pidof hkd", "r");
	if (cache == NULL) {
		fprintf(stderr, "%s: failed to read hkd pid\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	char pid_str[20];
	fgets(pid_str, 20, cache);
	pid = strtoul(pid_str, NULL, 10);
	fclose(cache);
	if (pid == 0) {
		fprintf(stderr, "%s: hkd not running\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	/* parse options */
	int opt;
	while ((opt = getopt(argc, argv, "h")) != -1) {
		switch (opt) {
			case 'h':
				print_usage(argv[0]);
				exit(EXIT_SUCCESS);
			case '?':
				fprintf(stderr, "%s: invalid option: %c\n", argv[0], opt);
				print_usage(argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	/* create thread for each device*/
	int count = 0;
	const char *device;
	while ((device = devices[count]) != NULL) count++;
	pthread_t relayers[count];

	int i = 0;
	while ((device = devices[i]) != NULL) {
		pthread_create(&relayers[i], NULL, handle_device, (void*)devices[i]);
		i++;
	}

	void *status;
	for (i = 0; i < count; i++) pthread_join(relayers[i], &status);

	return EXIT_SUCCESS;
}
