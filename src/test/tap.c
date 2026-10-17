/* tap: injects one finger tap at (x, y) into the touch panel's event node, or
 * a power button press, so the UI can be driven over SSH.
 * Usage: tap x y   or   tap drag x0 y0 x1 y1 [steps]   or   tap power */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>

static void emit(int fd, int type, int code, int value) {
    struct input_event e;
    memset(&e, 0, sizeof e);
    e.type = type; e.code = code; e.value = value;
    write(fd, &e, sizeof e);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "power") == 0) {
        int fd = open(argc > 2 ? argv[2] : "/dev/input/event0", O_WRONLY);
        if (fd < 0) return 1;
        emit(fd, EV_KEY, KEY_POWER, 1);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        usleep(80000);
        emit(fd, EV_KEY, KEY_POWER, 0);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        return 0;
    }
    if (argc >= 6 && strcmp(argv[1], "drag") == 0) {
        int fd = open("/dev/input/event1", O_WRONLY), steps = argc > 6 ? atoi(argv[6]) : 30;
        int x0 = atoi(argv[2]), y0 = atoi(argv[3]), x1 = atoi(argv[4]), y1 = atoi(argv[5]);
        if (fd < 0) return 1;
        emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 78);
        for (int i = 0; i <= steps; i++) {
            emit(fd, EV_ABS, ABS_MT_POSITION_X, x0 + (x1 - x0) * i / steps);
            emit(fd, EV_ABS, ABS_MT_POSITION_Y, y0 + (y1 - y0) * i / steps);
            emit(fd, EV_SYN, SYN_REPORT, 0);
            usleep(12000);
        }
        emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
        emit(fd, EV_SYN, SYN_REPORT, 0);
        return 0;
    }
    if (argc < 3) return 2;
    int fd = open(argc > 3 ? argv[3] : "/dev/input/event1", O_WRONLY);
    if (fd < 0) return 1;
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, 77);
    emit(fd, EV_ABS, ABS_MT_POSITION_X, atoi(argv[1]));
    emit(fd, EV_ABS, ABS_MT_POSITION_Y, atoi(argv[2]));
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(80000);
    emit(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    return 0;
}
