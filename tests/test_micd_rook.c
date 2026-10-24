/*
 * micd built for the Echo Spot (rook, -DLE_TARGET_ROOK): its microphones are
 * PCM 22, 16 kHz, six 24-bit lanes -- four microphones (0-3) behind two
 * TLV320AIC3101 ADCs (A, B) and the playback loopback (4, 5).
 */
#include "adapter/adapter.h"

#include <math.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PI 3.14159265358979323846
#define SECONDS 2
#define MIC_AMPLITUDE 4000.0
#define LOOPBACK_AMPLITUDE 30000.0

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

/* 1 kHz on the four microphones, 3 kHz on the loopback lanes. */
static int write_capture(const char *path, int mics_on)
{
    FILE *file = fopen(path, "wb");
    long i;

    if (!file)
        return -1;
    for (i = 0; i < 16000L * SECONDS; ++i) {
        unsigned char frame[18];
        double mic = mics_on ? MIC_AMPLITUDE * sin(2 * PI * 1000 * i / 16000) : 0;
        double loop = LOOPBACK_AMPLITUDE * sin(2 * PI * 3000 * i / 16000);
        int c;

        for (c = 0; c < 6; ++c) {
            long v = lrint(c < 4 ? mic : loop) * 256;
            frame[c * 3] = (unsigned char)(v & 0xff);
            frame[c * 3 + 1] = (unsigned char)((v >> 8) & 0xff);
            frame[c * 3 + 2] = (unsigned char)((v >> 16) & 0xff);
        }
        if (fwrite(frame, 1, sizeof(frame), file) != sizeof(frame)) {
            fclose(file);
            return -1;
        }
    }
    return fclose(file);
}

/* Amplitude of the f Hz component of n samples (single-bin DFT). */
static double tone(const short *s, size_t n, double f)
{
    double re = 0, im = 0;
    size_t i;

    for (i = 0; i < n; ++i) {
        re += s[i] * cos(2 * PI * f * (double)i / 16000);
        im += s[i] * sin(2 * PI * f * (double)i / 16000);
    }
    return 2 * sqrt(re * re + im * im) / (double)n;
}

static int read_file(const char *path, char *buffer, size_t size)
{
    FILE *file = fopen(path, "r");
    size_t used;

    if (!file)
        return -1;
    used = fread(buffer, 1, size - 1, file);
    fclose(file);
    buffer[used] = '\0';
    return 0;
}

/* stream_mono's samples after its JSON reply, up to max. */
static size_t stream_mono(const char *socket_path, short *out, size_t max)
{
    static char buffer[16000 * SECONDS * 2 + 4096];
    const char request[] = "{\"v\":1,\"id\":7,\"cmd\":\"stream_mono\",\"args\":{}}\n";
    struct sockaddr_un address;
    size_t used = 0;
    ssize_t count;
    char *body;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0)
        return 0;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
        write(fd, request, sizeof(request) - 1) != (ssize_t)(sizeof(request) - 1)) {
        close(fd);
        return 0;
    }
    while (used < sizeof(buffer) &&
           (count = read(fd, buffer + used, sizeof(buffer) - used)) > 0)
        used += (size_t)count;
    close(fd);
    body = memchr(buffer, '\n', used);
    if (!body || !strstr(buffer, "\"ok\":true"))
        return 0;
    ++body;
    used = (size_t)(buffer + used - body) / 2;
    if (used > max)
        used = max;
    memcpy(out, body, used * 2);
    return used;
}

int main(void)
{
    static short samples[16000 * SECONDS];
    char directory[128], socket_path[160], capture_path[160], capture_args[160];
    char mixer_path[160], mixer_args[160], pcm_path[160], response[LE_ADAPTER_MSG_MAX];
    struct timespec pause_time = {0, 20000000};
    struct le_adapter *adapter = NULL;
    pid_t child = -1;
    size_t n;
    int result = 0;
    int i;

    snprintf(directory, sizeof(directory), "/tmp/libreecho-micd-rook-%ld", (long)getpid());
    snprintf(socket_path, sizeof(socket_path), "%s/mic.sock", directory);
    snprintf(capture_path, sizeof(capture_path), "%s/fake-tinycap", directory);
    snprintf(capture_args, sizeof(capture_args), "%s/capture.args", directory);
    snprintf(mixer_path, sizeof(mixer_path), "%s/fake-tinymix", directory);
    snprintf(mixer_args, sizeof(mixer_args), "%s/mixer.args", directory);
    snprintf(pcm_path, sizeof(pcm_path), "%s/capture.s24", directory);
    CHECK(mkdir(directory, 0700) == 0);
    {
        FILE *file = fopen(capture_path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file, "#!/bin/sh\nprintf '%%s\\n' \"$@\" > '%s'\ncat '%s'\n",
                      capture_args, pcm_path) > 0);
        CHECK(fclose(file) == 0);
        CHECK(chmod(capture_path, 0700) == 0);
        file = fopen(mixer_path, "w");
        CHECK(file != NULL);
        CHECK(fprintf(file, "#!/bin/sh\nprintf '%%s\\n' \"$@\" >> '%s'\n", mixer_args) > 0);
        CHECK(fclose(file) == 0);
        CHECK(chmod(mixer_path, 0700) == 0);
    }
    CHECK(write_capture(pcm_path, 1) == 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        /* Unity digital gain (default 4x), so the mix itself is measured. */
        if (setenv("LE_MIC_DIGITAL_GAIN", "100", 1))
            _exit(127);
        execl("./build-rook/libreecho-micd", "libreecho-micd", "--foreground",
              "--socket", socket_path, "--idme-dir", "/missing-idme",
              "--dt-idme-dir", "/missing-dt-idme", "--pcm-path", "/dev/null",
              "--capture-bin", capture_path, "--mixer-bin", mixer_path, (char *)NULL);
        _exit(127);
    }
    for (i = 0; i < 100 && !adapter; ++i) {
        adapter = le_adapter_connect(socket_path, 250);
        if (!adapter)
            nanosleep(&pause_time, NULL);
    }
    CHECK(adapter != NULL);
    CHECK(le_adapter_call(adapter, "status", "{}", response, sizeof(response)) == LE_ADAPTER_OK);
    CHECK(strstr(response, "\"device\":22") != NULL);
    CHECK(strstr(response, "\"raw_channels\":6,\"active_microphone_channels\":4") != NULL);
    CHECK(strstr(response, "\"inactive_transport_channels\":[4,5]") != NULL);
    CHECK(strstr(response, "\"adc_codecs\":2") != NULL);
    CHECK(strstr(response, "\"beamforming\":false") != NULL);
    puts("micd rook: status describes PCM 22, four microphones and the loopback lanes");
    le_adapter_close(adapter);
    adapter = le_adapter_connect(socket_path, 250);
    CHECK(adapter != NULL);
    CHECK(le_adapter_call(adapter, "stream_raw", "{\"channel\":6}",
                          response, sizeof(response)) == LE_ADAPTER_ERR_REJECTED);
    le_adapter_close(adapter);
    adapter = NULL;

    n = stream_mono(socket_path, samples, sizeof(samples) / sizeof(samples[0]));
    CHECK(n > 16000);
    CHECK(read_file(capture_args, response, sizeof(response)) == 0);
    CHECK(strcmp(response, "--\n-D\n0\n-d\n22\n-c\n6\n-r\n16000\n-b\n24\n-p\n640\n-n\n4\n") == 0);
    puts("micd rook: capture opens PCM 22 with six lanes");
    CHECK(read_file(mixer_args, response, sizeof(response)) == 0);
    CHECK(strstr(response, "ADC_A MICPGA Volume Ctrl") != NULL);
    CHECK(strstr(response, "ADC_B MICPGA Volume Ctrl") != NULL);
    CHECK(strstr(response, "ADC_C") == NULL);
    CHECK(strstr(response, "ADC_D") == NULL);
    puts("micd rook: the mixer profile names only ADCs A and B");
    /* Skip the first half second (high-pass settling). */
    {
        double mic = tone(samples + 8000, n - 8000, 1000);
        double loop = tone(samples + 8000, n - 8000, 3000);

        fprintf(stderr, "mono: 1 kHz %.0f, 3 kHz %.0f\n", mic, loop);
        CHECK(mic > MIC_AMPLITUDE * 0.8 && mic < MIC_AMPLITUDE * 1.2);
        CHECK(loop < 100);
    }
    puts("micd rook: mono stream is the four microphones, not the loopback");

cleanup:
    if (adapter)
        le_adapter_close(adapter);
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
    }
    unlink(socket_path);
    unlink(capture_path);
    unlink(capture_args);
    unlink(mixer_path);
    unlink(mixer_args);
    unlink(pcm_path);
    rmdir(directory);
    return result;
}
