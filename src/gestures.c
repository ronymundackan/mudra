/* gestures.c
 *
 * Detect gestures with libinput and map them to custom actions (volume,
 * media controls, tab switching, etc).
 *
 * Compile:
 *   gcc -o gestures gestures.c -ludev -linput -lm
 *
 * Notes:
 * - Uses fork+execvp for safe command execution.
 * - Sets SIGCHLD to SIG_IGN so child processes are auto-reaped (fire-and-forget).
 * - Example commands assume tools like pactl, playerctl, xdotool are installed.
 */

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
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

/* --- libinput restricted open/close --- */
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

/* --- Global state used for swipe accumulation --- */
double total_dx = 0;
double total_dy = 0;

/* --- Action definitions --- */
enum Action {
    ACT_NONE = 0,
    ACT_VOL_UP,
    ACT_VOL_DOWN,
    ACT_PLAY_PAUSE,
    ACT_NEXT_TRACK,
    ACT_PREV_TRACK,
    ACT_SWITCH_TAB_NEXT,
    ACT_SWITCH_TAB_PREV,
    ACT_SHOW_OVERVIEW, /* placeholder */
};

/* --- Command definitions (argv arrays, NULL-terminated) --- */
/* Volume via pactl */
static char *const cmd_pactl_vol_up[]   = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", "+5%", NULL};
static char *const cmd_pactl_vol_down[] = {"pactl", "set-sink-volume", "@DEFAULT_SINK@", "-5%", NULL};

/* Media via playerctl (MPRIS) */
static char *const cmd_playerctl_playpause[] = {"playerctl", "play-pause", NULL};
static char *const cmd_playerctl_next[]      = {"playerctl", "next", NULL};
static char *const cmd_playerctl_prev[]      = {"playerctl", "previous", NULL};

/* X11 key simulation using xdotool (won't work on Wayland) */
static char *const cmd_xdotool_ctrl_tab[]       = {"xdotool", "key", "ctrl+Tab", NULL};
static char *const cmd_xdotool_ctrl_shift_tab[] = {"xdotool", "key", "ctrl+Shift+Tab", NULL};
static char *const cmd_xdotool_alt_tab[]        = {"xdotool", "key", "Alt+Tab", NULL};

/* Example sway command (Wayland compositor sway) to show workspace 1 (customize) */
static char *const cmd_sway_show_overview[] = {"swaymsg", "exec", "gnome-shell --overview", NULL}; /* placeholder */

/* --- Safe, non-blocking command execution --- */
/* Fire-and-forget: fork and execvp. Parent does not wait (SIGCHLD=SIG_IGN set in main). */
static void run_command(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return;
    }
    if (pid == 0) {
        /* Child process: replace image with command */
        /* It's safer to execvp directly (no shell). */
        execvp(argv[0], argv);
        /* If execvp returns, it failed. */
        fprintf(stderr, "exec failed for %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    /* Parent: do not wait here; SIGCHLD=SIG_IGN will prevent zombies. */
}

/* --- High-level trigger: choose a command based on environment and action --- */
void trigger_action(enum Action a) {
    const char *wayland = getenv("WAYLAND_DISPLAY");
    const char *xdisplay = getenv("DISPLAY");

    switch (a) {
        case ACT_VOL_UP:
            run_command(cmd_pactl_vol_up);
            break;
        case ACT_VOL_DOWN:
            run_command(cmd_pactl_vol_down);
            break;
        case ACT_PLAY_PAUSE:
            run_command(cmd_playerctl_playpause);
            break;
        case ACT_NEXT_TRACK:
            run_command(cmd_playerctl_next);
            break;
        case ACT_PREV_TRACK:
            run_command(cmd_playerctl_prev);
            break;
        case ACT_SWITCH_TAB_NEXT:
            if (xdisplay) {
                run_command(cmd_xdotool_ctrl_tab);
            } else if (wayland) {
                /* Wayland injection is compositor-specific; user must provide a command or use ydotool/seat API */
                fprintf(stderr, "SWITCH_TAB_NEXT: Wayland key injection not configured.\n");
            } else {
                fprintf(stderr, "SWITCH_TAB_NEXT: no DISPLAY or WAYLAND found.\n");
            }
            break;
        case ACT_SWITCH_TAB_PREV:
            if (xdisplay) {
                run_command(cmd_xdotool_ctrl_shift_tab);
            } else if (wayland) {
                fprintf(stderr, "SWITCH_TAB_PREV: Wayland key injection not configured.\n");
            } else {
                fprintf(stderr, "SWITCH_TAB_PREV: no DISPLAY or WAYLAND found.\n");
            }
            break;
        case ACT_SHOW_OVERVIEW:
            if (wayland) {
                /* placeholder; may not be correct for your environment */
                run_command(cmd_sway_show_overview);
            } else {
                /* example: Alt+Tab on X11 (not real "overview") */
                if (xdisplay) run_command(cmd_xdotool_alt_tab);
            }
            break;
        default:
            fprintf(stderr, "trigger_action: unknown action %d\n", a);
            break;
    }
}

/* --- Gesture -> Action mapping helpers --- */
/* Map a swipe's finger count and direction (LEFT/RIGHT/UP/DOWN) to actions. */
static void map_and_trigger_swipe(int finger_count, const char *direction) {
    if (!direction) return;

    /* 3-finger gestures: media + volume */
    if (finger_count == 3) {
        if (strcmp(direction, "LEFT") == 0) {
            trigger_action(ACT_PREV_TRACK);
        } else if (strcmp(direction, "RIGHT") == 0) {
            trigger_action(ACT_NEXT_TRACK);
        } else if (strcmp(direction, "UP") == 0) {
            trigger_action(ACT_VOL_UP);
        } else if (strcmp(direction, "DOWN") == 0) {
            trigger_action(ACT_VOL_DOWN);
        }
    }
    /* 4-finger gestures: tab/window management */
    else if (finger_count == 4) {
        if (strcmp(direction, "LEFT") == 0) {
            trigger_action(ACT_SWITCH_TAB_PREV);
        } else if (strcmp(direction, "RIGHT") == 0) {
            trigger_action(ACT_SWITCH_TAB_NEXT);
        } else if (strcmp(direction, "UP") == 0) {
            trigger_action(ACT_SHOW_OVERVIEW); /* show overview or workspace switch */
        } else if (strcmp(direction, "DOWN") == 0) {
            /* customize as needed */
            fprintf(stderr, "4-finger DOWN: no action mapped (customize me)\n");
        }
    } else {
        /* other finger counts: no mapping by default */
        fprintf(stderr, "map_and_trigger_swipe: %d-finger swipe %s -> no mapping\n", finger_count, direction);
    }
}

/* Map hold/tap end (treated as tap) */
static void map_and_trigger_tap(int finger_count) {
    if (finger_count == 3) {
        trigger_action(ACT_PLAY_PAUSE);
    } else if (finger_count == 4) {
        /* 4-finger tap -> play/pause as well (example), customize as required */
        trigger_action(ACT_PLAY_PAUSE);
    } else {
        fprintf(stderr, "map_and_trigger_tap: %d-finger tap -> no mapping\n", finger_count);
    }
}

/* --- Gesture event handlers (integrated with mapping) --- */
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

        const char *direction = (fabs(total_dx) > fabs(total_dy)) 
            ? ((total_dx > 0) ? "RIGHT" : "LEFT") 
            : ((total_dy > 0) ? "DOWN" : "UP");

        printf("ACTION: %d-Finger Swipe %s\n", finger_count, direction);

        /* Map & trigger */
        map_and_trigger_swipe(finger_count, direction);
    }
}

void handle_hold_tap(struct libinput_event *event) {
    struct libinput_event_gesture *gesture = libinput_event_get_gesture_event(event);
    enum libinput_event_type type = libinput_event_get_type(event);
    int finger_count = libinput_event_gesture_get_finger_count(gesture);

    /* We trigger the action only when the hold ENDS (meaning you lifted your fingers) */
    if (type == LIBINPUT_EVENT_GESTURE_HOLD_END) {
        if (libinput_event_gesture_get_cancelled(gesture)) {
            printf("debug: %d-finger gesture cancelled (moved too much?)\n", finger_count);
            return;
        }

        printf("ACTION: %d-Finger Tap detected\n", finger_count);

        /* Map & trigger */
        map_and_trigger_tap(finger_count);
    }
}

/* --- Main loop --- */
int main(void) {
    /* Make children auto-reaped to avoid waiting in main loop (fire-and-forget commands) */
    signal(SIGCHLD, SIG_IGN);

    struct udev *udev;
    struct libinput *li;
    struct pollfd fds;

    udev = udev_new();
    if (!udev) {
        fprintf(stderr, "Failed to create udev context\n");
        return 1;
    }

    li = libinput_udev_create_context(&interface, NULL, udev);
    if (!li) {
        fprintf(stderr, "Failed to create libinput context\n");
        udev_unref(udev);
        return 1;
    }

    if (libinput_udev_assign_seat(li, "seat0") != 0) {
        fprintf(stderr, "Failed to assign seat0\n");
        libinput_unref(li);
        udev_unref(udev);
        return 1;
    }

    fds.fd = libinput_get_fd(li);
    fds.events = POLLIN;
    fds.revents = 0;

    printf("Precision Pad Backend Running (Event Mode)...\n");

    while (poll(&fds, 1, -1) > -1) {
        libinput_dispatch(li);
        struct libinput_event *event;

        while ((event = libinput_get_event(li))) {
            enum libinput_event_type type = libinput_event_get_type(event);

            /* Filter 1: Swipes */
            if (type >= LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN &&
                type <= LIBINPUT_EVENT_GESTURE_SWIPE_END) {
                handle_swipe(event);
            }
            /* Filter 2: Holds (used here as taps) */
            else if (type == LIBINPUT_EVENT_GESTURE_HOLD_BEGIN ||
                     type == LIBINPUT_EVENT_GESTURE_HOLD_END) {
                handle_hold_tap(event);
            }

            libinput_event_destroy(event);
            /* libinput_dispatch(li);  -- dispatch will be called at top of loop */
        }
    }

    /* Clean up (we normally exit loop via signal) */
    libinput_unref(li);
    udev_unref(udev);

    return 0;
}

