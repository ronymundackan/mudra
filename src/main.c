#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <libudev.h>
#include <libinput.h>
#include <sys/poll.h>

/* --- Helper Functions --- */
static int open_restricted(const char *path, int flags, void *user_data) {
    (void)user_data;
    int fd = open(path, flags);
    return fd < 0 ? -errno : fd;
}
static void close_restricted(int fd, void *user_data) {
    (void)user_data;
    close(fd);
}
const struct libinput_interface interface = {
    .open_restricted = open_restricted,
    .close_restricted = close_restricted,
};

/* --- State Variables --- */
double total_dx = 0;
double total_dy = 0;

/* --- Logic --- */

void handle_swipe(struct libinput_event *event) {
    struct libinput_event_gesture *gesture = libinput_event_get_gesture_event(event);
    enum libinput_event_type type = libinput_event_get_type(event);
    int finger_count = libinput_event_gesture_get_finger_count(gesture);

    if (type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN) {
        total_dx = 0; total_dy = 0;
    } 
    else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE) {
        total_dx += libinput_event_gesture_get_dx(gesture);
        total_dy += libinput_event_gesture_get_dy(gesture);
    } 
    else if (type == LIBINPUT_EVENT_GESTURE_SWIPE_END) {
        if (libinput_event_gesture_get_cancelled(gesture)) return;
        
        char *direction = (fabs(total_dx) > fabs(total_dy)) 
            ? ((total_dx > 0) ? "RIGHT" : "LEFT") 
            : ((total_dy > 0) ? "DOWN" : "UP");

        printf("ACTION: %d-Finger Swipe %s\n", finger_count, direction);
    }
}

void handle_hold_tap(struct libinput_event *event) {
    struct libinput_event_gesture *gesture = libinput_event_get_gesture_event(event);
    enum libinput_event_type type = libinput_event_get_type(event);
    int finger_count = libinput_event_gesture_get_finger_count(gesture);

    // We trigger the action only when the hold ENDS (meaning you lifted your fingers)
    if (type == LIBINPUT_EVENT_GESTURE_HOLD_END) {
        if (libinput_event_gesture_get_cancelled(gesture)) {
            printf("debug: %d-finger gesture cancelled (moved too much?)\n", finger_count);
            return;
        }

        // Based on your logs, 3 and 4 finger 'taps' are showing up as Hold events.
        printf("ACTION: %d-Finger Tap detected\n", finger_count);
    }
}

int main(void) {
    struct udev *udev;
    struct libinput *li;
    struct pollfd fds;

    udev = udev_new();
    li = libinput_udev_create_context(&interface, NULL, udev);
    libinput_udev_assign_seat(li, "seat0");
    
    fds.fd = libinput_get_fd(li);
    fds.events = POLLIN;
    fds.revents = 0;

    printf("Precision Pad Backend Running (Event Mode)...\n");

    while (poll(&fds, 1, -1) > -1) {
        libinput_dispatch(li);
        struct libinput_event *event;

        while ((event = libinput_get_event(li))) {
            enum libinput_event_type type = libinput_event_get_type(event);

            // Filter 1: Swipes (unchanged)
            if (type >= LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN && 
                type <= LIBINPUT_EVENT_GESTURE_SWIPE_END) {
                handle_swipe(event);
            }
            
            // Filter 2: Holds (Your "Taps")
            // Note: These constants are available in newer libinput versions (1.19+)
            else if (type == LIBINPUT_EVENT_GESTURE_HOLD_BEGIN || 
                     type == LIBINPUT_EVENT_GESTURE_HOLD_END) {
                handle_hold_tap(event);
            }

            libinput_event_destroy(event);
            libinput_dispatch(li);
        }
    }
    return 0;
}
