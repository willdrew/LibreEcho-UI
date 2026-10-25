/* audiod built for the Echo Spot (-DLE_TARGET_ROOK). The engine's taper was set for the Dot's small
 * speaker; on the Spot's far hotter amplifier even the engine's quietest master step (-60 dB) was too
 * loud for music, and logical 20 distorted (owner, 2026-10-25). The engine cannot be replaced (a
 * rebuilt one crashed the kernel), so audiod spreads the owner's 1..100 over a target level, linear
 * in dB from LE_AUDIO_SPOT_MIN_DB (-80) to LE_AUDIO_SPOT_MAX_DB (-49): the engine's master gets the
 * lowest step at or above the target (never below its -60 dB floor), and music (media.volume) the
 * rest as a trim, so music follows the target exactly. Speech and chimes follow the master alone.
 * 0 stays mute. The owner's own number is kept beside master.volume so a restart shows it again. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define LE_TARGET_ROOK 1
#define LE_MASTER_VOLUME_DIR "/tmp/libreecho-audiod-rook-volume"
#define LE_MASTER_VOLUME_PATH LE_MASTER_VOLUME_DIR "/master.volume"
#define CODEC_REF_FILE "/tmp/libreecho-audiod-rook-codec-ref"
#define main audiod_program_main
#include "../src/adapter/audiod.c"
#undef main

#include <math.h>
#include <stdio.h>

static int failures;

static void expect(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "audiod rook volume: %s\n", what);
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
    double trim, prev_level = -1000.0;
    int v, step, prev_step = 0;
    char what[96];

    unsetenv("LE_AUDIO_SPOT_MIN_DB");
    unsetenv("LE_AUDIO_SPOT_MAX_DB");
    expect(rook_engine_step(0, &trim) == 0 && trim == 0.0, "0 is mute");
    expect(fabs(rook_engine_db(1) + 60.0) < 0.6, "the engine's step 1 is -60 dB");
    for (v = 1; v <= 100; ++v) {
        double want = -80.0 + 31.0 * (v - 1) / 99.0, level;

        step = rook_engine_step(v, &trim);
        level = rook_engine_db(step) + trim;
        snprintf(what, sizeof(what), "volume %d: music at %.1f dB, want %.1f", v, level, want);
        expect(fabs(level - want) < 0.05, what);
        snprintf(what, sizeof(what), "volume %d: trim %.1f dB is a cut", v, trim);
        expect(trim <= 0.0, what);
        snprintf(what, sizeof(what), "volume %d: rises (step %d after %d)", v, step, prev_step);
        expect(level > prev_level && step >= prev_step && step >= 1, what);
        prev_level = level;
        prev_step = step;
    }
    expect(rook_engine_step(10, &trim) == 1, "volume 10 sits on the engine's floor");
    step = rook_engine_step(100, &trim);
    snprintf(what, sizeof(what), "the top: step %d, under the step 20 that distorted", step);
    expect(step >= 18 && step < 20, what);
    setenv("LE_AUDIO_SPOT_MAX_DB", "-40", 1);
    expect(fabs(rook_engine_db(rook_engine_step(100, &trim)) + trim + 40.0) < 0.05, "the top from the environment");
    setenv("LE_AUDIO_SPOT_MAX_DB", "-10", 1);
    expect(fabs(rook_engine_db(rook_engine_step(100, &trim)) + trim + 30.0) < 0.05, "the top is never above -30 dB");
    unsetenv("LE_AUDIO_SPOT_MAX_DB");

    /* The codec's own volume (PCM Playback Volume, 0.5 dB a step, 127 = 0 dB) stays at the engine's
     * reference, 127, unless spot-start.sh has installed an engine that checks for a lower one and
     * says so in LE_AUDIO_CODEC_REF_FILE: then audiod sets the codec there, and the digital levels
     * rise by the same amount, so music lands where it did and speech, held at the engine's
     * floor, comes down with the codec. */
    setenv("LE_AUDIO_CODEC_REF_FILE", CODEC_REF_FILE, 1);
    unlink(CODEC_REF_FILE);
    expect(rook_codec_ref() == 127, "no file: the codec at 0 dB");
    expect(speaker_value_for_percent(0, 175, 100) == 127, "no file: audiod writes 127");
    put(CODEC_REF_FILE, "79\n");
    expect(rook_codec_ref() == 79, "the file: 79");
    expect(speaker_value_for_percent(0, 175, 100) == 79, "the file: audiod writes 79");
    for (v = 1; v <= 100; ++v) {
        double want = -80.0 + 31.0 * (v - 1) / 99.0 + 24.0;

        step = rook_engine_step(v, &trim);
        snprintf(what, sizeof(what), "codec -24 dB, volume %d: digital %.1f, want %.1f", v,
                 rook_engine_db(step) + trim, want);
        expect(fabs(rook_engine_db(step) + trim - want) < 0.05 && trim <= 0.0, what);
    }
    put(CODEC_REF_FILE, "200\n");
    expect(rook_codec_ref() == 127, "a bad file: 127");
    put(CODEC_REF_FILE, "10\n");
    expect(rook_codec_ref() == 127, "below the floor: 127");
    unlink(CODEC_REF_FILE);

    /* A restart reads master.volume (the engine's step) back: the owner's number beside it wins
     * when it still matches; otherwise the nearest owner number. */
    mkdir(LE_MASTER_VOLUME_DIR, 0755);
    memset(&audio, 0, sizeof(audio));
    snprintf(what, sizeof(what), "%d\n", rook_engine_step(40, &trim));
    put(LE_MASTER_VOLUME_PATH, what);
    put(LE_MASTER_VOLUME_PATH ".owner", "40\n");
    expect(audio_load_logical_volume(&audio) == 0 && audio.volume == 40, "the owner's number comes back");
    unlink(LE_MASTER_VOLUME_PATH ".owner");
    snprintf(what, sizeof(what), "%d\n", rook_engine_step(100, &trim));
    put(LE_MASTER_VOLUME_PATH, what);
    expect(audio_load_logical_volume(&audio) == 0 && audio.volume == 100, "no owner number: the nearest");
    unlink(LE_MASTER_VOLUME_PATH);
    rmdir(LE_MASTER_VOLUME_DIR);
    if (!failures)
        puts("audiod rook volume: ok");
    return failures != 0;
}
