/* Host regression test: real home renderer, RAM framebuffer and mocked I/O.
 * No framebuffer device, backlight or userstore is opened by this test. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static FILE *fixture_fopen(const char *, const char *);
static int fixture_ioctl(int, unsigned long, ...);
#define fopen fixture_fopen
#define ioctl fixture_ioctl
#define main home_main
#include "../ui/home.c"
#undef main
#undef ioctl
#undef fopen

static int level, maximum = 10000, writes, saves, max_reads, fail_write, fail_save;
static int sends, waits, waveform, full, monochrome, blank;
static char written[64], saved[64];
static unsigned checks;

static void check(int ok, const char *what) {
    ++checks;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

static FILE *fixture_fopen(const char *path, const char *mode) {
    static char contents[64];
    int backlight = strncmp(path, "/sys/class/backlight/", 21) == 0;
    int brightness = backlight && strstr(path, "/brightness");
    int limit = backlight && strstr(path, "/max_brightness");
    int persist = strcmp(path, "/mnt/us/linux/light") == 0;
    if (strcmp(mode, "w") == 0 && (brightness || persist)) {
        if ((brightness && fail_write) || (persist && fail_save)) { errno = EIO; return NULL; }
        if (brightness) { ++writes; return fmemopen(written, sizeof written, "w"); }
        ++saves; return fmemopen(saved, sizeof saved, "w");
    }
    assert(strcmp(mode, "r") == 0);
    if (brightness) snprintf(contents, sizeof contents, "%d\n", writes ? atoi(written) : level * level);
    else if (limit) { ++max_reads; snprintf(contents, sizeof contents, "%d\n", maximum); }
    else if (persist) snprintf(contents, sizeof contents, "%d\n", saves ? atoi(saved) : level * level);
    else { fprintf(stderr, "Unexpected I/O: %s\n", path); abort(); }
    return fmemopen(contents, strlen(contents), "r");
}

static int fixture_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    va_list args;
    va_start(args, request);
    void *arg = va_arg(args, void *);
    va_end(args);
    if (request == MXCFB_WAIT_FOR_UPDATE_COMPLETE) {
        check(((unsigned *)arg)[0] == marker, "wait uses current marker");
        ++waits; return 0;
    }
    struct mxcfb_rect region;
    if (board_pw4()) {
        check(request == MXCFB_SEND_UPDATE_PW4, "PW4 update layout");
        struct mxcfb_update_data_pw4 *u = arg;
        region = u->region; waveform = u->waveform; full = u->mode;
    } else {
        check(request == MXCFB_SEND_UPDATE, "PW3 update layout");
        struct mxcfb_update_data *u = arg;
        region = u->region; waveform = u->waveform; full = u->mode;
    }
    check(region.left == (unsigned)slider_r.x && region.top == (unsigned)slider_r.y &&
          region.width == (unsigned)slider_r.w && region.height == (unsigned)slider_r.h,
          "refresh restricted to slider");
    ++sends;
    monochrome = blank = 1;
    for (int y = slider_r.y; y < slider_r.y + slider_r.h; ++y)
        for (int x = slider_r.x; x < slider_r.x + slider_r.w; ++x) {
            unsigned char pixel = fb[y * PITCH + x];
            if (pixel != BLACK && pixel != WHITE) monochrome = 0;
            if (pixel != WHITE) blank = 0;
        }
    if (waveform == (board_pw4() ? 6 : WF_A2)) check(monochrome, "A2 framebuffer contains only B&W");
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    board_is_pw4 = atoi(argv[2]);
    FILE *stream = fopen(argv[1], "rb");
    assert(stream);
    fseek(stream, 0, SEEK_END);
    long size = ftell(stream);
    rewind(stream);
    unsigned char *font_data = malloc((size_t)size);
    assert(font_data && fread(font_data, 1, (size_t)size, stream) == (size_t)size);
    fclose(stream);
    assert(stbtt_InitFont(&f_ui_bold.info, font_data, 0));
    f_ui_bold.loaded = 1;
    W = 1072; H = 1448; PITCH = 1088;
    size_t bytes = (size_t)PITCH * H;
    unsigned char *memory = malloc(bytes + 128), *expected = malloc(bytes);
    assert(memory && expected);
    memset(memory, 0xA5, bytes + 128);
    fb = memory + 64;
    unsigned char *live = fb;
    napps = 13; layout();
    const int levels[] = {0, 100, 31, 80, 1, 99, 50, 10, 0, 100, 31};
    for (unsigned i = 0; i < sizeof levels / sizeof *levels; ++i) {
        level = levels[i]; sends = waits = 0;
        redraw_slider(SLIDER_DOWN);
        /* after the first round, entry also waits for the release before it */
        check(sends == 1 && waits == (i ? 2 : 1) && waveform == (board_pw4() ? 4 : WF_GC16), "single fenced grayscale-to-B&W entry");
        check(monochrome && !blank, "entry draws content directly, without a white intermediate frame");
        check(full == (board_pw4() ? 1 : 0), "entry retains board-specific non-flashing mapping");
        sends = waits = 0; level = 100 - levels[i];
        redraw_slider(SLIDER_MOVE);
        check(sends == 1 && waits == 1 && waveform == (board_pw4() ? 6 : WF_A2) && !full, "fenced A2 drag after monochrome entry");
        sends = waits = 0;
        redraw_slider(SLIDER_UP);
        check(sends == 1 && waits == 0 && slider_pending == marker && waveform == (board_pw4() ? 4 : WF_GC16),
              "grayscale release is not waited for, and is remembered for the next frame");
        check(full == (board_pw4() ? 1 : 0), "release does not request flashing GC16");
        memset(expected, 0xA5, bytes);
        fb = expected; draw_slider(); fb = live;
        check(memcmp(expected, live, bytes) == 0, "final frame matches fresh render, unrelated pixels unchanged");
        for (int j = 0; j < 64; ++j)
            check(memory[j] == 0xA5 && memory[bytes + 64 + j] == 0xA5, "framebuffer guards unchanged");
    }
    level = writes = saves = max_reads = 0;
    light_begin();
    int middle = slider_x0 + (slider_x1 - slider_x0) / 2;
    check(light_from_x(middle), "changed percentage refreshes");
    check(writes == 1 && saves == 0 && max_reads == 1, "cache maximum and do not save during drag");
    check(light_level() == 50 && max_reads == 1, "render from cached brightness");
    check(!light_from_x(middle) && writes == 1, "duplicate hardware level skipped");
    check(!light_from_x(middle + 1) && writes == 2, "sub-percent hardware change skips duplicate render");
    light_finish(); light_finish();
    check(saves == 1 && atoi(saved) == atoi(written), "save only once on release");
    light_begin(); light_finish();
    check(saves == 1, "unchanged gesture does not save");
    light_begin(); fail_write = 1;
    check(!light_from_x(slider_x1) && light_level() == 50, "failed hardware write does not advance state");
    fail_write = 0;
    check(light_from_x(slider_x1 + 10000) && light_applied == maximum, "clamp maximum");
    check(light_from_x(slider_x0 - 10000) && light_applied == 0, "clamp minimum");
    int old_saved = light_saved;
    fail_save = 1; light_finish(); fail_save = 0;
    check(light_saved == old_saved && saves == 1, "failed persistence is not acknowledged");
    maximum = 0; light_begin();
    check(!light_active && !light_from_x(middle), "invalid maximum rejected");
    coalesce_moves = 1; evq_head = evq_tail = 0;
    evq_push((event){EV_DOWN, 0, 0, 0, 0});
    for (int j = 0; j < 1000; ++j) evq_push((event){EV_MOVE, j, 0, j, 0});
    evq_push((event){EV_TAP, 0, 0, 999, 0});
    evq_push((event){EV_POWER, 0, 0, 0, 0});
    check(evq_tail == 4 && evq[1].x == 999 && evq[2].type == EV_TAP && evq[3].type == EV_POWER,
          "coalesce 1000 moves without losing release or power");
    evq_head = evq_tail = EVQ - 2;
    evq_push((event){EV_DOWN, 0, 0, 0, 0});
    for (int j = 0; j < 1000; ++j) evq_push((event){EV_MOVE, j, 0, j, 0});
    evq_push((event){EV_TAP, 0, 0, 999, 0});
    check(evq_tail == 1 && evq[EVQ - 1].x == 999 && evq[0].type == EV_TAP, "coalescing across ring wrap");
    coalesce_moves = 0; evq_head = evq_tail = 0;
    evq_push((event){EV_MOVE, 1, 0, 1, 0}); evq_push((event){EV_MOVE, 2, 0, 2, 0});
    check(evq_tail == 2, "other gestures keep original queue behavior");
    for (int j = 0; j < GLYPH_CACHE; ++j) free(glyph_cache[j].bm);
    free(font_data); free(expected); free(memory);
    printf("PASS mocked %s: %u checks; no physical device validation\n", board_name(), checks);
    return 0;
}
