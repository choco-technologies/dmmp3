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
 *   cover.mp3      0.5 s of 440 Hz, 22050 Hz mono, ID3v2.3 with the front cover: a 16x16 JPEG
 *                  of 765 bytes at 92 (ffmpeg -i ... -i cover.jpg -map 0:a -map 1:v -c:v copy)
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

/* ---- Pictures: tags made here, the "images" in them only some bytes ---- */

static uint8_t g_tag[1024];
static uint32_t g_tag_length;

static void put(const void* data, uint32_t size)
{
    memcpy(g_tag + g_tag_length, data, size);
    g_tag_length += size;
}

static void put_size(uint32_t size, bool syncsafe)
{
    uint8_t b[4];
    for (int i = 0; i < 4; i++)
        b[i] = syncsafe ? (uint8_t)((size >> (7 * (3 - i))) & 0x7Fu) : (uint8_t)(size >> (8 * (3 - i)));
    put(b, 4);
}

/* A tag of that version (2, 3, 4) and flags begins */
static void tag_begin(uint8_t version, uint8_t flags)
{
    const uint8_t h[6] = { 'I', 'D', '3', version, 0, flags };
    g_tag_length = 0;
    put(h, sizeof(h));
    put_size(0, true);                                  /* (written by tag_write()) */
}

/* A frame of the tag; its data a string literal */
#define FRAME(id, flags, data)  put_frame(id, flags, data, sizeof(data) - 1U)

static void put_frame(const char* id, uint8_t flags, const void* data, uint32_t size)
{
    if (g_tag[3] == 2)
    {
        const uint8_t s[3] = { (uint8_t)(size >> 16), (uint8_t)(size >> 8), (uint8_t)size };
        put(id, 3);
        put(s, 3);
    }
    else
    {
        const uint8_t f[2] = { 0, flags };
        put(id, 4);
        put_size(size, g_tag[3] == 4);
        put(f, 2);
    }
    put(data, size);
}

/* The tag, its padding and some bytes after it, into the file `name` */
static const char* tag_write(const char* name)
{
    static char path[128];
    static const uint8_t after[32] = { 0 };
    uint32_t length = g_tag_length;
    g_tag_length = 6;
    put_size(length - 10U + 16U, true);                 /* With 16 bytes of padding */
    g_tag_length = length;
    Dmod_SnPrintf(path, sizeof(path), "%s/%s", DMMP3_TEMP, name);
    void* f = Dmod_FileOpen(path, "wb");
    if (f != NULL)
    {
        Dmod_FileWrite(g_tag, 1, g_tag_length, f);
        Dmod_FileWrite(after, 1, sizeof(after), f);
        Dmod_FileClose(f);
    }
    return path;
}

/* Whether the picture found is the image `image` (a string literal) */
static bool is_image(const char* path, const dmmp3_picture_t* p, const char* image)
{
    char got[32] = { 0 };
    void* f = Dmod_FileOpen(path, "rb");
    bool ok = f != NULL && p->size == strlen(image) && p->size < sizeof(got) &&
              Dmod_FileSeek(f, (Dmod_FileOffset_t)p->offset, DMOD_SEEK_SET) == 0 &&
              Dmod_FileRead(got, 1, p->size, f) == p->size && memcmp(got, image, p->size) == 0;
    if (f != NULL)
        Dmod_FileClose(f);
    if (!ok)
        Dmod_Printf("    the picture at %u (%u bytes) is \"%s\", not \"%s\"\n", (unsigned)p->offset, (unsigned)p->size, got, image);
    return ok;
}

DMOD_TEST_STEP(dmmp3_finds_the_front_cover)
{
    /* As ffmpeg writes it */
    dmmp3_picture_t p;
    dmmp3_info_t info;
    uint8_t ends[4];
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(FIXTURE("cover.mp3"), &p), 0);
    DMOD_TEST_EXPECT_EQ(p.type, DMMP3_PICTURE_FRONT);
    DMOD_TEST_EXPECT_EQ(p.offset, 92u);
    DMOD_TEST_EXPECT_EQ(p.size, 765u);
    void* f = Dmod_FileOpen(FIXTURE("cover.mp3"), "rb");
    DMOD_TEST_EXPECT_NOT_NULL(f);
    if (f != NULL)
    {
        (void)Dmod_FileSeek(f, (Dmod_FileOffset_t)p.offset, DMOD_SEEK_SET);
        (void)Dmod_FileRead(ends, 1, 2, f);
        (void)Dmod_FileSeek(f, (Dmod_FileOffset_t)(p.offset + p.size - 2U), DMOD_SEEK_SET);
        (void)Dmod_FileRead(ends + 2, 1, 2, f);
        Dmod_FileClose(f);
        DMOD_TEST_EXPECT_TRUE(ends[0] == 0xFF && ends[1] == 0xD8 && ends[2] == 0xFF && ends[3] == 0xD9);     /* SOI ... EOI */
    }
    DMOD_TEST_EXPECT_EQ(dmmp3_read_info(FIXTURE("cover.mp3"), &info), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(info.title, "With Cover"), 0);
    DMOD_TEST_EXPECT_TRUE(near(info.duration_ms, 500, 120));

    /* 2.3: another picture first, the front cover with a UTF-16 description ("Co") */
    tag_begin(3, 0);
    FRAME("TIT2", 0, "\x00" "Covers");
    FRAME("APIC", 0, "\x00" "image/png\x00" "\x00" "\x00" "PNG-OTHER");
    FRAME("APIC", 0, "\x01" "image/jpeg\x00" "\x03" "\xFF\xFE" "C\x00o\x00" "\x00\x00" "JPEG-FRONT");
    FRAME("TPE1", 0, "\x00" "After");
    const char* path = tag_write("covers23.mp3");
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(path, &p), 0);
    DMOD_TEST_EXPECT_EQ(p.type, DMMP3_PICTURE_FRONT);
    DMOD_TEST_EXPECT_TRUE(is_image(path, &p, "JPEG-FRONT"));
    memset(&info, 0, sizeof(info));
    (void)dmmp3_read_info(path, &info);                 /* (no audio: only its tags) */
    DMOD_TEST_EXPECT_EQ(strcmp(info.title, "Covers"), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(info.artist, "After"), 0);

    /* 2.2: PIC, a format in place of the MIME type */
    tag_begin(2, 0);
    FRAME("TT2", 0, "\x00" "Old");
    FRAME("PIC", 0, "\x00" "JPG" "\x03" "front\x00" "JPEG-22");
    path = tag_write("covers22.mp3");
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(path, &p), 0);
    DMOD_TEST_EXPECT_EQ(p.type, DMMP3_PICTURE_FRONT);
    DMOD_TEST_EXPECT_TRUE(is_image(path, &p, "JPEG-22"));
}

DMOD_TEST_STEP(dmmp3_finds_a_picture_stored_as_it_is)
{
    /* 2.4: the front cover unsynchronised (not given), then the band's with a group and a length before it */
    dmmp3_picture_t p;
    tag_begin(4, 0);
    FRAME("APIC", 0x02, "\x00" "image/jpeg\x00" "\x03" "\x00" "JPEG\xFF\x00" "SYNC");
    FRAME("APIC", 0x41, "\x07" "\x00\x00\x00\x14" "\x03" "image/png\x00" "\x08" "band\x00" "PNG-BAND");
    const char* path = tag_write("covers24.mp3");
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(path, &p), 0);
    DMOD_TEST_EXPECT_EQ(p.type, 8);
    DMOD_TEST_EXPECT_TRUE(is_image(path, &p, "PNG-BAND"));

    /* 2.3 unsynchronised as a whole, compressed: none */
    tag_begin(3, 0x80);
    FRAME("APIC", 0, "\x00" "image/png\x00" "\x03" "\x00" "PNG-UNSYNC");
    path = tag_write("unsync23.mp3");
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(path, &p), -ENODATA);
    tag_begin(3, 0);
    FRAME("APIC", 0x80, "\x00\x00\x00\x20" "x\x9C" "zlib");
    path = tag_write("zlib23.mp3");
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(path, &p), -ENODATA);

    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(FIXTURE("vbr_tags.mp3"), &p), -ENODATA);    /* Tags, no picture */
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(FIXTURE("cbr_mono.mp3"), &p), -ENODATA);    /* No tag */
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(FIXTURE("none.mp3"), &p), -ENOENT);
    DMOD_TEST_EXPECT_EQ(dmmp3_find_picture(NULL, &p), -EINVAL);
}
