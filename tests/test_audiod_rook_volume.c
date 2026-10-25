/* audiod built for the Echo Spot (-DLE_TARGET_ROOK): the engine's volume taper was set for the
 * Dot's small speaker, and on the Spot's louder amplifier logical 10 was already near full and 20
 * distorted the speaker (owner report, 2026-10-25). So on the Spot the owner's 0..100 is scaled
 * into 0..LE_AUDIO_VOLUME_MAX (default 12) before it reaches the engine; 0 stays mute, any level
 * above 0 is at least 1. The owner's own number is kept beside master.volume so a restart shows
 * it again, not a rounded-back one. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_TARGET_ROOK 1
#define LE_MASTER_VOLUME_DIR "/tmp/libreecho-audiod-rook-volume"
#define LE_MASTER_VOLUME_PATH LE_MASTER_VOLUME_DIR "/master.volume"
#define VOLUME_MAX_FILE "/tmp/libreecho-audiod-rook-volume-max"
#define main audiod_program_main
#include "../src/adapter/audiod.c"
#undef main

#include <stdio.h>

static int failures;

static void expect(int got, int want, const char *what)
{
    if (got != want) {
        fprintf(stderr, "audiod rook volume: %s: got %d, want %d\n", what, got, want);
        ++failures;
    }
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

int main(void)
{
    struct audio_hw audio;

    unsetenv("LE_AUDIO_VOLUME_MAX");
    setenv("LE_AUDIO_VOLUME_MAX_FILE", VOLUME_MAX_FILE, 1);
    unlink(VOLUME_MAX_FILE);
    expect(rook_engine_volume(0), 0, "0 is mute");
    expect(rook_engine_volume(100), 12, "100 is the safe top");
    expect(rook_engine_volume(50), 6, "50 is half of it");
    expect(rook_engine_volume(10), 1, "10 is the quietest step");
    expect(rook_engine_volume(3), 1, "anything above 0 is heard");
    setenv("LE_AUDIO_VOLUME_MAX", "20", 1);
    expect(rook_engine_volume(100), 20, "the top from the environment");
    setenv("LE_AUDIO_VOLUME_MAX", "500", 1);
    expect(rook_engine_volume(100), 100, "the top is never above 100");
    setenv("LE_AUDIO_VOLUME_MAX", "junk", 1);
    expect(rook_engine_volume(100), 12, "a bad top falls back to 12");
    unsetenv("LE_AUDIO_VOLUME_MAX");
    /* The top from a file (spot-start.sh writes it once the engine's trim is in place), read each
     * time; the environment still wins; a bad file falls back to the safe default. */
    put(VOLUME_MAX_FILE, "50\n");
    expect(rook_engine_volume(100), 50, "the top from the file");
    expect(rook_engine_volume(10), 5, "the file's scale");
    setenv("LE_AUDIO_VOLUME_MAX", "20", 1);
    expect(rook_engine_volume(100), 20, "the environment beats the file");
    unsetenv("LE_AUDIO_VOLUME_MAX");
    put(VOLUME_MAX_FILE, "junk\n");
    expect(rook_engine_volume(100), 12, "a bad file falls back to 12");
    unlink(VOLUME_MAX_FILE);

    /* A restart reads master.volume (the engine's level) back: the owner's number beside it wins
     * when it still matches; otherwise the nearest owner number. */
    mkdir(LE_MASTER_VOLUME_DIR, 0755);
    memset(&audio, 0, sizeof(audio));
    put(LE_MASTER_VOLUME_PATH, "5\n");
    put(LE_MASTER_VOLUME_PATH ".owner", "40\n");
    expect(audio_load_logical_volume(&audio), 0, "load");
    expect(audio.volume, 40, "the owner's number comes back");
    put(LE_MASTER_VOLUME_PATH, "6\n");
    expect(audio_load_logical_volume(&audio), 0, "load stale");
    expect(audio.volume, 50, "a stale owner number gives way to the nearest");
    unlink(LE_MASTER_VOLUME_PATH ".owner");
    put(LE_MASTER_VOLUME_PATH, "12\n");
    expect(audio_load_logical_volume(&audio), 0, "load bare");
    expect(audio.volume, 100, "no owner number: the nearest");
    unlink(LE_MASTER_VOLUME_PATH);
    rmdir(LE_MASTER_VOLUME_DIR);
    if (!failures)
        puts("audiod rook volume: ok");
    return failures != 0;
}
