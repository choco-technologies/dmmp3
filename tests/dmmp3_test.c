#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmmp3.h"
#include <errno.h>
#include <string.h>

/*
 * The fixtures (made with ffmpeg / lame):
 *   cbr_mono.mp3   1 s of 440 Hz, 22050 Hz mono, 32 kbit/s, no tags, no Xing header
 *   vbr_tags.mp3   2 s of 1000 Hz, 44100 Hz stereo, VBR with a Xing header, ID3v2.3 (UTF-16 texts)
 *   id3v1.mp3      1 s of 440 Hz, 48000 Hz stereo, 64 kbit/s, an ID3v1 tag only
 */

#define FIXTURE(name)   DMMP3_FIXTURES "/" name

static int16_t g_pcm[4096 * 2];

static bool near(uint32_t value, uint32_t expected, uint32_t tolerance)
{
    bool ok = value + tolerance >= expected && value <= expected + tolerance;
    if (!ok)
        Dmod_Printf("    %u, not %u (+-%u)\n", (unsigned)value, (unsigned)expected, (unsigned)tolerance);
    return ok;
}

/* Every frame of a file; the sign changes of its first channel between `from` and `to` frames */
static uint32_t decode_all(dmmp3_t m, uint32_t from, uint32_t to, uint32_t* crossings)
{
    dmmp3_info_t info;
    dmmp3_get_info(m, &info);
    uint32_t total = 0;
    int16_t last = 0;
    *crossings = 0;
    for (;;)
    {
        int32_t n = dmmp3_read(m, g_pcm, 4096);
        if (n <= 0)
            break;
        for (int32_t i = 0; i < n; i++, total++)
        {
            int16_t s = g_pcm[i * info.channels];
            if (total >= from && total < to && ((s >= 0) != (last >= 0)))
                (*crossings)++;
            last = s;
        }
    }
    return total;
}

DMOD_TEST_STEP(dmmp3_reads_the_stream_and_its_length)
{
    dmmp3_info_t info;
    DMOD_TEST_EXPECT_EQ(dmmp3_read_info(FIXTURE("cbr_mono.mp3"), &info), 0);
    DMOD_TEST_EXPECT_EQ(info.sample_rate_hz, 22050u);
    DMOD_TEST_EXPECT_EQ(info.channels, 1);
    DMOD_TEST_EXPECT_EQ(info.bitrate_kbps, 32);
    DMOD_TEST_EXPECT_FALSE(info.vbr);
    DMOD_TEST_EXPECT_TRUE(near(info.duration_ms, 1000, 120));      /* From the bit rate */
    DMOD_TEST_EXPECT_EQ(info.title[0], '\0');

    DMOD_TEST_EXPECT_EQ(dmmp3_read_info(FIXTURE("vbr_tags.mp3"), &info), 0);
    DMOD_TEST_EXPECT_EQ(info.sample_rate_hz, 44100u);
    DMOD_TEST_EXPECT_EQ(info.channels, 2);
    DMOD_TEST_EXPECT_TRUE(info.vbr);
    DMOD_TEST_EXPECT_TRUE(near(info.duration_ms, 2000, 60));       /* From the Xing header */
    DMOD_TEST_EXPECT_TRUE(info.bitrate_kbps > 0);
}

DMOD_TEST_STEP(dmmp3_reads_the_tags)
{
    dmmp3_info_t info;
    DMOD_TEST_EXPECT_EQ(dmmp3_read_info(FIXTURE("vbr_tags.mp3"), &info), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(info.title, "Za\xC5\xBC\xC3\xB3\xC5\x82\xC4\x87 g\xC4\x99\xC5\x9Bl\xC4\x85"), 0);   /* UTF-16 into UTF-8 */
    DMOD_TEST_EXPECT_EQ(strcmp(info.artist, "Synthwave Rider"), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(info.album, "Neon"), 0);

    DMOD_TEST_EXPECT_EQ(dmmp3_read_info(FIXTURE("id3v1.mp3"), &info), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(info.title, "Old Tag"), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(info.artist, "V1"), 0);              /* Its spaces trimmed */
    DMOD_TEST_EXPECT_EQ(info.album[0], '\0');
    DMOD_TEST_EXPECT_EQ(info.sample_rate_hz, 48000u);
    DMOD_TEST_EXPECT_TRUE(near(info.duration_ms, 1000, 120));       /* Without the tag's bytes */
}

DMOD_TEST_STEP(dmmp3_decodes_what_was_encoded)
{
    int status = -1;
    uint32_t crossings = 0;
    dmmp3_t m = dmmp3_open(FIXTURE("cbr_mono.mp3"), &status);
    DMOD_TEST_EXPECT_NOT_NULL(m);
    DMOD_TEST_EXPECT_EQ(status, 0);
    uint32_t frames = decode_all(m, 5512, 16538, &crossings);       /* 0.25 ... 0.75 s */
    DMOD_TEST_EXPECT_TRUE(near(frames, 22050, 2400));               /* (the encoder's delay and padding) */
    DMOD_TEST_EXPECT_TRUE(near(crossings, 440, 6));                 /* 440 Hz: 880 sign changes a second */
    DMOD_TEST_EXPECT_TRUE(near(dmmp3_tell(m), frames * 1000u / 22050u, 1));
    DMOD_TEST_EXPECT_EQ(dmmp3_read(m, g_pcm, 100), 0);              /* The end */
    dmmp3_close(m);

    m = dmmp3_open(FIXTURE("vbr_tags.mp3"), &status);
    DMOD_TEST_EXPECT_NOT_NULL(m);
    frames = decode_all(m, 22050, 66150, &crossings);               /* 0.5 ... 1.5 s */
    DMOD_TEST_EXPECT_TRUE(near(frames, 88200, 2400));
    DMOD_TEST_EXPECT_TRUE(near(crossings, 2000, 10));
    dmmp3_close(m);
}

DMOD_TEST_STEP(dmmp3_seeks)
{
    int status = -1;
    uint32_t crossings = 0;
    dmmp3_t m = dmmp3_open(FIXTURE("vbr_tags.mp3"), &status);
    DMOD_TEST_EXPECT_NOT_NULL(m);
    DMOD_TEST_EXPECT_EQ(dmmp3_seek(m, 1000), 0);
    DMOD_TEST_EXPECT_EQ(dmmp3_tell(m), 1000u);
    uint32_t frames = decode_all(m, 4410, 26460, &crossings);       /* 0.1 ... 0.6 s after it */
    DMOD_TEST_EXPECT_TRUE(near(frames, 44100, 13000));              /* The other half - as close as the Xing table tells */
    DMOD_TEST_EXPECT_TRUE(near(crossings, 1000, 10));               /* Still the tone */
    dmmp3_info_t info;
    dmmp3_get_info(m, &info);
    DMOD_TEST_EXPECT_TRUE(near(dmmp3_tell(m), info.duration_ms, 300));  /* The time of what was left out counted */
    DMOD_TEST_EXPECT_EQ(dmmp3_seek(m, 0), 0);
    frames = decode_all(m, 0, 0, &crossings);
    DMOD_TEST_EXPECT_TRUE(near(frames, 88200, 2400));               /* All of it again */
    DMOD_TEST_EXPECT_EQ(dmmp3_seek(m, 99999), 0);                   /* Past the end: the end */
    DMOD_TEST_EXPECT_TRUE(dmmp3_read(m, g_pcm, 4096) < 1200);
    dmmp3_close(m);

    m = dmmp3_open(FIXTURE("cbr_mono.mp3"), &status);
    DMOD_TEST_EXPECT_EQ(dmmp3_seek(m, 500), 0);                     /* By the bit rate */
    frames = decode_all(m, 0, 0, &crossings);
    DMOD_TEST_EXPECT_TRUE(near(frames, 11025, 2400));
    dmmp3_close(m);
}

DMOD_TEST_STEP(dmmp3_refuses_what_is_not_mp3)
{
    int status = 0;
    dmmp3_info_t info;
    DMOD_TEST_EXPECT_NULL(dmmp3_open(FIXTURE("none.mp3"), &status));
    DMOD_TEST_EXPECT_EQ(status, -ENOENT);
    DMOD_TEST_EXPECT_EQ(dmmp3_read_info(FIXTURE("none.mp3"), &info), -ENOENT);

    void* f = Dmod_FileOpen(DMMP3_TEMP "/text.mp3", "wb");
    DMOD_TEST_EXPECT_NOT_NULL(f);
    const char text[] = "This is no MP3 file, only some text in it.\n";
    for (int i = 0; f != NULL && i < 20; i++)
        Dmod_FileWrite(text, 1, sizeof(text) - 1U, f);
    if (f != NULL)
        Dmod_FileClose(f);
    DMOD_TEST_EXPECT_NULL(dmmp3_open(DMMP3_TEMP "/text.mp3", &status));
    DMOD_TEST_EXPECT_EQ(status, -EBADMSG);

    DMOD_TEST_EXPECT_EQ(dmmp3_read(NULL, g_pcm, 1), -EINVAL);
    DMOD_TEST_EXPECT_EQ(dmmp3_get_info(NULL, &info), -EINVAL);
    dmmp3_close(NULL);
}
