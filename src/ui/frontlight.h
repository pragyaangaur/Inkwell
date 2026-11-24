/* Home's frontlight state for one gesture. Save to userstore only on lift,
 * and do not refresh a duplicate displayed percentage. */
#ifndef FRONTLIGHT_H
#define FRONTLIGHT_H
static int light_active, light_maximum, light_applied, light_saved;

static int light_percent(int value, int maximum) {
    return maximum > 0 && value > 0 ? (int)(sqrtf((float)value / maximum) * 100 + 0.5f) : 0;
}

static int light_write(const char *path, int value) {
    FILE *stream = fopen(path, "w");
    if (!stream) { perror(path); return 0; }
    int ok = fprintf(stream, "%d\n", value) > 0;
    if (fclose(stream) != 0) ok = 0;
    if (!ok) fprintf(stderr, "frontlight write failed: %s\n", path);
    return ok;
}

static void light_begin(void) {
    light_maximum = read_int(LIGHT_FILE("max_brightness"));
    light_applied = read_int(LIGHT_FILE("brightness"));
    light_saved = read_int("/mnt/us/linux/light");
    light_active = light_maximum > 0 && light_applied >= 0 && light_applied <= light_maximum;
}

static int light_from_x(int x) {
    if (!light_active || slider_x1 <= slider_x0) return 0;
    float fraction = (float)(x - slider_x0) / (slider_x1 - slider_x0);
    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    int value = (int)(fraction * fraction * light_maximum + 0.5f);
    if (value == light_applied) return 0;
    int previous = light_percent(light_applied, light_maximum);
    if (!light_write(LIGHT_FILE("brightness"), value)) return 0;
    light_applied = value;
    return previous != light_percent(value, light_maximum);
}

static void light_finish(void) {
    if (light_active && light_applied != light_saved && light_write("/mnt/us/linux/light", light_applied))
        light_saved = light_applied;
    light_active = 0;
}
#endif
