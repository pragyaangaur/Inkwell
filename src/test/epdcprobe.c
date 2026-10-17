/* Tries MXCFB_SEND_UPDATE with a few struct sizes and temperature values to
 * find the combination the PW3 kernel accepts. Draws a small grey box. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

struct rect { unsigned top, left, width, height; };
struct upd {
    struct rect r;
    unsigned wf, mode, marker, hist_bw, hist_gray;
    int temp;
    unsigned flags;
    unsigned alt[8];
};

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fd, FBIOGET_VSCREENINFO, &v); ioctl(fd, FBIOGET_FSCREENINFO, &f);
    printf("xres %u yres %u vx %u vy %u bpp %u rot %u gray %u line %u smem %u\n",
           v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel, v.rotate, v.grayscale, f.line_length, f.smem_len);
    unsigned char *fb = mmap(0, f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 100; y < 300; y++) memset(fb + (v.yoffset + y) * f.line_length + 100, 0x40, 200);
    unsigned sizes[] = {68, 72, 76, 80};
    int temps[] = {0x1000, 0x1001, 24};
    unsigned m = 1;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 3; j++) {
        struct upd u; memset(&u, 0, sizeof u);
        u.r = (struct rect){100, 100, 200, 200};
        u.wf = 2; u.mode = 1; u.marker = m++; u.hist_bw = 1; u.hist_gray = 2; u.temp = temps[j];
        unsigned req = 0x40000000u | (sizes[i] << 16) | ('F' << 8) | 0x2E;
        int r = ioctl(fd, req, &u);
        printf("size %u temp 0x%x req 0x%08x -> %d %s\n", sizes[i], temps[j], req, r, r ? strerror(errno) : "ok");
    }
    return 0;
}
