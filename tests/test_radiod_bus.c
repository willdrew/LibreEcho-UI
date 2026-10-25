/* radiod's media bus: a live stream's network hiccups must not empty the bus (the audio engine
 * closes the speaker stream after about 0.34 s without data, and the amplifier pops off and on;
 * the Echo Spot, 2026-10-25), so radiod opens it with a 512 KiB pipe (~2.7 s at 48 kHz stereo);
 * and a stop or pause must not leave that much playing, so it empties the bus then. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define main radiod_program_main
#include "../src/adapter/radiod.c"
#undef main

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main(void)
{
    char path[] = "/tmp/radiod-bus-XXXXXX", fifo[64], chunk[4096], byte;
    int reader, writer, i;

    CHECK(mkdtemp(path) != NULL);
    snprintf(fifo, sizeof(fifo), "%s/media.pcm", path);
    CHECK(mkfifo(fifo, 0600) == 0);
    reader = open(fifo, O_RDONLY | O_NONBLOCK);   /* the engine's end */
    CHECK(reader >= 0);
    writer = open_media_bus(fifo);
    CHECK(writer >= 0);
    CHECK(fcntl(writer, F_GETPIPE_SZ) >= 512 * 1024);
    memset(chunk, 1, sizeof(chunk));
    for (i = 0; i < 64; ++i)                       /* 256 KiB waiting, more than the old pipe held */
        CHECK(write(writer, chunk, sizeof(chunk)) == (ssize_t)sizeof(chunk));
    media_bus = fifo;
    flush_bus();
    CHECK(read(reader, &byte, 1) < 0 && errno == EAGAIN);
    close(writer);
    close(reader);
    unlink(fifo);
    rmdir(path);
    puts("radiod bus: ok");
    return 0;
}
