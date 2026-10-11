#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmmp3.h"
#include <errno.h>
#include <string.h>

/*
 * The decoder: minimp3 fed from a buffer of the file. Its scratch - 16 kB
 * that minimp3 keeps on the stack - is the decoder's (MINIMP3_SCRATCH), so
 * a thread with a small stack can decode.
 */

static void* scratch_of(void* dec);
#define MINIMP3_SCRATCH(dec)    scratch_of(dec)
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3        /* Layer III only: what MP3 files are */
#include "minimp3/minimp3.h"

#define BUFFER_SIZE     16384u      /* Of the file at once: minimp3 syncs on several frames */
#define MAX_TAG_FRAME   4096u       /* A tag text longer than this is not read (pictures are skipped) */
#define MP3_MAGIC       0x4D503344u /* 'MP3D' */

/* minimp3's decoder with its scratch (NULL: only the frames' headers are read) */
typedef struct
{
    mp3dec_t            dec;        /* First: minimp3 gets its address */
    mp3dec_scratch_t*   scratch;
} decoder_t;

static void* scratch_of(void* dec)
{
    return ((decoder_t*)dec)->scratch;
}

struct dmmp3
{
    uint32_t            magic;
    decoder_t           d;
    mp3dec_scratch_t    scratch;
    void*               file;
    uint32_t            audio_start;    /* After the ID3v2 tag and the Xing frame */
    uint32_t            audio_end;      /* Before the ID3v1 tag */
    uint32_t            data_bytes;     /* Xing: bytes of the stream, 0: not known */
    uint32_t            frame_samples;  /* Per frame (per channel) */
    uint8_t             toc[100];       /* Xing table of contents */
    bool                have_toc;
    dmmp3_info_t        info;

    uint8_t             buffer[BUFFER_SIZE];
    uint32_t            length, pos;    /* What the buffer holds, where decoding is in it */
    uint32_t            file_pos;       /* Of the end of the buffer */
    bool                eof;

    int16_t             pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint32_t            pcm_frames, pcm_pos;   /* Of the decoded frame: frames in it, given out */
    uint8_t             pcm_channels;
    uint64_t            frames_out;     /* Since the start (or the seek) */
    uint64_t            frames_base;    /* Where the seek put it */
};

/* ---- The file ---- */

static bool read_at(void* f, uint32_t at, void* buffer, size_t size)
{
    return Dmod_FileSeek(f, (Dmod_FileOffset_t)at, DMOD_SEEK_SET) == 0 && Dmod_FileRead(buffer, 1, size, f) == size;
}

static uint32_t be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t syncsafe(const uint8_t* p)
{
    return ((uint32_t)(p[0] & 0x7Fu) << 21) | ((uint32_t)(p[1] & 0x7Fu) << 14) | ((uint32_t)(p[2] & 0x7Fu) << 7) | (p[3] & 0x7Fu);
}

/* ---- Tags ---- */

/* One code point as UTF-8 at out + *at (out holds n bytes with the zero); false when it does not fit */
static bool put_utf8(char* out, size_t* at, size_t n, uint32_t c)
{
    char b[4];
    size_t k;
    if (c < 0x80u)
    {
        b[0] = (char)c;
        k = 1;
    }
    else if (c < 0x800u)
    {
        b[0] = (char)(0xC0u | (c >> 6));
        b[1] = (char)(0x80u | (c & 0x3Fu));
        k = 2;
    }
    else if (c < 0x10000u)
    {
        b[0] = (char)(0xE0u | (c >> 12));
        b[1] = (char)(0x80u | ((c >> 6) & 0x3Fu));
        b[2] = (char)(0x80u | (c & 0x3Fu));
        k = 3;
    }
    else
    {
        b[0] = (char)(0xF0u | (c >> 18));
        b[1] = (char)(0x80u | ((c >> 12) & 0x3Fu));
        b[2] = (char)(0x80u | ((c >> 6) & 0x3Fu));
        b[3] = (char)(0x80u | (c & 0x3Fu));
        k = 4;
    }
    if (*at + k + 1U > n)
        return false;
    memcpy(out + *at, b, k);
    *at += k;
    return true;
}

/* A tag text (its encoding byte first) as UTF-8, cut to DMMP3_TAG_SIZE */
static void tag_text(const uint8_t* p, uint32_t size, char* out)
{
    size_t at = 0;
    out[0] = '\0';
    if (size < 1)
        return;
    uint8_t encoding = p[0];
    p++;
    size--;
    if (encoding == 1 || encoding == 2)
    {
        /* UTF-16: with a BOM (1), big-endian (2) */
        bool big = encoding == 2;
        uint32_t i = 0;
        if (encoding == 1 && size >= 2 && ((p[0] == 0xFE && p[1] == 0xFF) || (p[0] == 0xFF && p[1] == 0xFE)))
        {
            big = p[0] == 0xFE;
            i = 2;
        }
        for (; i + 1U < size; i += 2)
        {
            uint32_t c = big ? ((uint32_t)p[i] << 8 | p[i + 1]) : ((uint32_t)p[i + 1] << 8 | p[i]);
            if (c == 0)
                break;
            if (c >= 0xD800u && c < 0xDC00u && i + 3U < size)
            {
                uint32_t lo = big ? ((uint32_t)p[i + 2] << 8 | p[i + 3]) : ((uint32_t)p[i + 3] << 8 | p[i + 2]);
                if (lo >= 0xDC00u && lo < 0xE000u)
                {
                    c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 2;
                }
            }
            if (!put_utf8(out, &at, DMMP3_TAG_SIZE, c))
                break;
        }
    }
    else
    {
        /* ISO-8859-1 (0), UTF-8 (3) */
        for (uint32_t i = 0; i < size && p[i] != 0; i++)
        {
            if (encoding == 3)
            {
                /* Whole characters only */
                uint32_t k = (p[i] < 0x80u) ? 1u : (p[i] < 0xE0u) ? 2u : (p[i] < 0xF0u) ? 3u : 4u;
                if (at + k + 1U > DMMP3_TAG_SIZE || i + k > size)
                    break;
                memcpy(out + at, p + i, k);
                at += k;
                i += k - 1U;
            }
            else if (!put_utf8(out, &at, DMMP3_TAG_SIZE, p[i]))
                break;
        }
    }
    out[at] = '\0';
}

/* ID3v1 pads with spaces */
static void trim(char* s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1U] == ' ')
        s[--n] = '\0';
}

/* ID3v2 at the start: its tags; returns the bytes it takes (0: none) */
static uint32_t read_id3v2(void* f, dmmp3_info_t* info)
{
    uint8_t h[10];
    if (!read_at(f, 0, h, sizeof(h)) || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4)
        return 0;
    uint8_t version = h[3];
    uint32_t size = syncsafe(h + 6);
    uint32_t end = 10u + size + (((h[5] & 0x10u) != 0) ? 10u : 0u);     /* (with a footer) */
    uint32_t at = 10;
    if (version >= 3 && (h[5] & 0x40u) != 0)
    {
        /* An extended header: skipped */
        uint8_t e[4];
        if (!read_at(f, at, e, 4))
            return end;
        at += (version == 4) ? syncsafe(e) : be32(e) + 4u;
    }
    uint8_t* text = Dmod_Malloc(MAX_TAG_FRAME);
    uint32_t header = (version == 2) ? 6u : 10u;
    while (text != NULL && at + header <= 10u + size)
    {
        uint8_t fh[10];
        uint32_t fsize;
        char id[5] = { 0 };
        if (!read_at(f, at, fh, header) || fh[0] == 0)
            break;                                      /* The padding */
        if (version == 2)
        {
            memcpy(id, fh, 3);
            fsize = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
        }
        else
        {
            memcpy(id, fh, 4);
            fsize = (version == 4) ? syncsafe(fh + 4) : be32(fh + 4);
        }
        at += header;
        char* to = (strcmp(id, "TIT2") == 0 || strcmp(id, "TT2") == 0) ? info->title :
                   (strcmp(id, "TPE1") == 0 || strcmp(id, "TP1") == 0) ? info->artist :
                   (strcmp(id, "TALB") == 0 || strcmp(id, "TAL") == 0) ? info->album : NULL;
        if (to != NULL && fsize > 0 && fsize <= MAX_TAG_FRAME && read_at(f, at, text, fsize))
            tag_text(text, fsize, to);
        if (fsize > size)
            break;
        at += fsize;                                    /* (a picture: skipped, never read) */
    }
    if (text != NULL)
        Dmod_Free(text);
    return end;
}

/* ID3v1 at the end: what ID3v2 did not give; returns the bytes it takes (0 or 128) */
static uint32_t read_id3v1(void* f, uint32_t file_size, dmmp3_info_t* info)
{
    uint8_t t[128];
    if (file_size < 128u || !read_at(f, file_size - 128u, t, sizeof(t)) || memcmp(t, "TAG", 3) != 0)
        return 0;
    char* fields[3] = { info->title, info->artist, info->album };
    for (uint32_t k = 0; k < 3; k++)
    {
        if (fields[k][0] != '\0')
            continue;
        uint8_t latin[31];
        latin[0] = 0;                                   /* ISO-8859-1 */
        memcpy(latin + 1, t + 3 + 30 * k, 30);
        tag_text(latin, 31, fields[k]);
        trim(fields[k]);
    }
    return 128;
}

/* ---- The stream ---- */

typedef struct
{
    uint32_t    first;          /* Of the first frame */
    uint32_t    first_bytes;
    uint32_t    samples;        /* Per frame */
    bool        xing;           /* The first frame is a Xing / Info frame - no audio */
    uint32_t    frames, bytes;  /* Xing: 0 when not given */
    uint8_t     toc[100];
    bool        have_toc;
} stream_t;

/* The first frame at `start`, and a Xing / Info header in it */
static int probe(void* f, uint32_t start, uint32_t end, dmmp3_info_t* info, stream_t* s)
{
    uint32_t n = (end - start > BUFFER_SIZE) ? BUFFER_SIZE : end - start;
    uint8_t* buf = Dmod_Malloc((n > 0) ? n : 1u);
    decoder_t* d = Dmod_Malloc(sizeof(decoder_t));
    int ret = (buf == NULL || d == NULL) ? -ENOMEM : (n == 0 || !read_at(f, start, buf, n)) ? -EBADMSG : 0;
    memset(s, 0, sizeof(*s));
    if (ret == 0)
    {
        mp3dec_frame_info_t fi;
        memset(&fi, 0, sizeof(fi));
        mp3dec_init(&d->dec);
        d->scratch = NULL;                              /* (no samples: not used) */
        int samples = mp3dec_decode_frame(&d->dec, buf, (int)n, NULL, &fi);
        if (samples <= 0 || fi.frame_bytes <= fi.frame_offset || fi.hz <= 0)
            ret = -EBADMSG;
        else
        {
            s->samples = (uint32_t)samples;
            s->first = start + (uint32_t)fi.frame_offset;
            s->first_bytes = (uint32_t)(fi.frame_bytes - fi.frame_offset);
            info->sample_rate_hz = (uint32_t)fi.hz;
            info->channels = (uint8_t)fi.channels;
            info->bitrate_kbps = (uint16_t)fi.bitrate_kbps;

            /* Xing / Info: after the side information */
            const uint8_t* h = buf + fi.frame_offset;
            bool mpeg1 = ((h[1] >> 3) & 3u) == 3u, mono = (h[3] >> 6) == 3u;
            uint32_t at = (uint32_t)fi.frame_offset + 4u + (mpeg1 ? (mono ? 17u : 32u) : (mono ? 9u : 17u));
            const uint8_t* x = buf + at;
            if (at + 8u <= n && (memcmp(x, "Xing", 4) == 0 || memcmp(x, "Info", 4) == 0))
            {
                uint32_t flags = be32(x + 4), k = at + 8u;
                s->xing = true;
                info->vbr = memcmp(x, "Xing", 4) == 0;
                if ((flags & 1u) != 0 && k + 4u <= n)
                {
                    s->frames = be32(buf + k);
                    k += 4;
                }
                if ((flags & 2u) != 0 && k + 4u <= n)
                {
                    s->bytes = be32(buf + k);
                    k += 4;
                }
                if ((flags & 4u) != 0 && k + 100u <= n)
                {
                    memcpy(s->toc, buf + k, 100);
                    s->have_toc = true;
                }
            }
        }
    }
    if (buf != NULL)
        Dmod_Free(buf);
    if (d != NULL)
        Dmod_Free(d);
    return ret;
}

/* The tags, the stream, the length: of an open file */
static int read_file_info(void* f, dmmp3_info_t* info, stream_t* s, uint32_t* end)
{
    size_t size = 0;
    if (!Dmod_FileSizeToSizeT(Dmod_FileSize(f), &size) || size > 0xFFFFFFFFu)
        return -EBADMSG;
    memset(info, 0, sizeof(*info));
    uint32_t start = read_id3v2(f, info);
    *end = (uint32_t)size - read_id3v1(f, (uint32_t)size, info);
    if (start >= *end)
        return -EBADMSG;
    int ret = probe(f, start, *end, info, s);
    if (ret != 0)
        return ret;
    uint32_t audio_start = s->first + (s->xing ? s->first_bytes : 0u);
    uint32_t audio = (*end > audio_start) ? *end - audio_start : 0u;
    if (s->frames != 0)
    {
        info->duration_ms = (uint32_t)((uint64_t)s->frames * s->samples * 1000u / info->sample_rate_hz);
        uint32_t bytes = (s->bytes != 0) ? s->bytes : audio;
        if (info->duration_ms > 0)
            info->bitrate_kbps = (uint16_t)((uint64_t)bytes * 8u / info->duration_ms);
    }
    else if (info->bitrate_kbps > 0)
        info->duration_ms = (uint32_t)((uint64_t)audio * 8u / info->bitrate_kbps);
    return 0;
}

dmod_dmmp3_api_declaration(1.0, int, _read_info, ( const char* path, dmmp3_info_t* info ))
{
    if (path == NULL || info == NULL)
        return -EINVAL;
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return -ENOENT;
    stream_t s;
    uint32_t end;
    int ret = read_file_info(f, info, &s, &end);
    Dmod_FileClose(f);
    return ret;
}

/* ---- Decoding ---- */

static bool is_mp3(dmmp3_t m)
{
    return m != NULL && m->magic == MP3_MAGIC;
}

dmod_dmmp3_api_declaration(1.0, dmmp3_t, _open, ( const char* path, int* status ))
{
    int dummy;
    if (status == NULL)
        status = &dummy;
    if (path == NULL)
    {
        *status = -EINVAL;
        return NULL;
    }
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
    {
        *status = -ENOENT;
        return NULL;
    }
    struct dmmp3* m = Dmod_Malloc(sizeof(*m));
    if (m == NULL)
    {
        Dmod_FileClose(f);
        *status = -ENOMEM;
        return NULL;
    }
    memset(m, 0, sizeof(*m));
    stream_t s;
    uint32_t end = 0;
    int ret = read_file_info(f, &m->info, &s, &end);
    if (ret != 0)
    {
        Dmod_FileClose(f);
        Dmod_Free(m);
        *status = ret;
        return NULL;
    }
    m->magic = MP3_MAGIC;
    m->file = f;
    m->d.scratch = &m->scratch;
    mp3dec_init(&m->d.dec);
    m->audio_start = s.first + (s.xing ? s.first_bytes : 0u);  /* (the Xing frame holds no audio) */
    m->audio_end = end;
    m->data_bytes = s.bytes;
    m->frame_samples = s.samples;
    m->have_toc = s.have_toc;
    memcpy(m->toc, s.toc, sizeof(m->toc));
    m->file_pos = m->audio_start;
    *status = 0;
    return m;
}

dmod_dmmp3_api_declaration(1.0, void, _close, ( dmmp3_t mp3 ))
{
    if (!is_mp3(mp3))
        return;
    Dmod_FileClose(mp3->file);
    mp3->magic = 0;
    Dmod_Free(mp3);
}

dmod_dmmp3_api_declaration(1.0, int, _get_info, ( dmmp3_t mp3, dmmp3_info_t* info ))
{
    if (!is_mp3(mp3) || info == NULL)
        return -EINVAL;
    *info = mp3->info;
    return 0;
}

/* More of the file into the buffer, after what is left of it */
static int refill(dmmp3_t m)
{
    if (m->pos > 0)
    {
        memmove(m->buffer, m->buffer + m->pos, m->length - m->pos);
        m->length -= m->pos;
        m->pos = 0;
    }
    uint32_t room = BUFFER_SIZE - m->length;
    uint32_t left = (m->file_pos < m->audio_end) ? m->audio_end - m->file_pos : 0u;
    uint32_t n = (room < left) ? room : left;
    if (left == 0)
    {
        m->eof = true;
        return 0;
    }
    if (n == 0)
        return 0;
    if (Dmod_FileSeek(m->file, (Dmod_FileOffset_t)m->file_pos, DMOD_SEEK_SET) != 0)
        return -EIO;
    size_t got = Dmod_FileRead(m->buffer + m->length, 1, n, m->file);
    if (got == 0)
        return -EIO;
    m->length += (uint32_t)got;
    m->file_pos += (uint32_t)got;
    return 0;
}

/* The next frame's samples into m->pcm: 1, 0 at the end, or an error */
static int decode_next(dmmp3_t m)
{
    for (;;)
    {
        if (m->length - m->pos < BUFFER_SIZE / 2u && !m->eof)
        {
            int ret = refill(m);
            if (ret != 0)
                return ret;
        }
        if (m->pos >= m->length)
            return 0;
        mp3dec_frame_info_t fi;
        memset(&fi, 0, sizeof(fi));
        int samples = mp3dec_decode_frame(&m->d.dec, m->buffer + m->pos, (int)(m->length - m->pos), m->pcm, &fi);
        if (fi.frame_bytes <= 0)
        {
            /* Not a whole frame in what is left: more of the file, or the end */
            uint32_t had = m->length - m->pos;
            int ret = m->eof ? 0 : refill(m);
            if (ret != 0)
                return ret;
            if (m->length - m->pos == had)
            {
                if (m->eof || m->file_pos >= m->audio_end)
                    return 0;
                m->pos = m->length;                     /* (a full buffer that is no frame: past it) */
            }
            continue;
        }
        m->pos += (uint32_t)fi.frame_bytes;
        if (samples > 0)
        {
            m->pcm_frames = (uint32_t)samples;
            m->pcm_pos = 0;
            m->pcm_channels = (uint8_t)fi.channels;
            return 1;
        }
        /* Skipped: not a frame, or one that needed the reservoir before a seek - its time goes on */
        if (fi.hz > 0)
            m->frames_out += m->frame_samples;
    }
}

dmod_dmmp3_api_declaration(1.0, int32_t, _read, ( dmmp3_t mp3, int16_t* pcm, uint32_t frames ))
{
    if (!is_mp3(mp3) || (pcm == NULL && frames != 0))
        return -EINVAL;
    uint32_t done = 0;
    uint8_t out_channels = mp3->info.channels;
    while (done < frames)
    {
        if (mp3->pcm_pos >= mp3->pcm_frames)
        {
            int ret = decode_next(mp3);
            if (ret < 0)
            {
                if (done > 0)
                    break;
                return ret;
            }
            if (ret == 0)
                break;
        }
        uint32_t n = mp3->pcm_frames - mp3->pcm_pos;
        if (n > frames - done)
            n = frames - done;
        const int16_t* src = mp3->pcm + mp3->pcm_pos * mp3->pcm_channels;
        int16_t* dst = pcm + done * out_channels;
        if (mp3->pcm_channels == out_channels)
            memcpy(dst, src, n * out_channels * sizeof(int16_t));
        else
        {
            /* A frame of other channels than the first one's (rare): mapped */
            for (uint32_t i = 0; i < n; i++)
            {
                if (out_channels == 1)
                    dst[i] = (int16_t)(((int32_t)src[2 * i] + src[2 * i + 1]) / 2);
                else
                    dst[2 * i] = dst[2 * i + 1] = src[i];
            }
        }
        mp3->pcm_pos += n;
        done += n;
    }
    mp3->frames_out += done;
    return (int32_t)done;
}

dmod_dmmp3_api_declaration(1.0, int, _seek, ( dmmp3_t mp3, uint32_t ms ))
{
    if (!is_mp3(mp3))
        return -EINVAL;
    uint32_t span = mp3->audio_end - mp3->audio_start;
    uint32_t duration = mp3->info.duration_ms;
    if (duration > 0 && ms > duration)
        ms = duration;
    uint64_t at;
    if (mp3->have_toc && duration > 0)
    {
        /* The table: for each percent of the time, a position in 1/256 of the stream */
        uint64_t per_mille = (uint64_t)ms * 1000u / duration;
        uint32_t p = (uint32_t)(per_mille / 10u);
        if (p > 99u)
            p = 99u;
        uint32_t a = mp3->toc[p], b = (p < 99u) ? mp3->toc[p + 1U] : 256u;
        uint64_t pos = (uint64_t)a * 10u + (uint64_t)((b > a) ? b - a : 0u) * (per_mille % 10u);  /* 1/2560 */
        uint32_t bytes = (mp3->data_bytes != 0) ? mp3->data_bytes : span;
        at = pos * bytes / 2560u;
    }
    else
        at = (uint64_t)ms * mp3->info.bitrate_kbps / 8u;                /* kbit/s = bytes per ms * 8 */
    if (at > span)
        at = span;
    mp3->file_pos = mp3->audio_start + (uint32_t)at;
    mp3->length = 0;
    mp3->pos = 0;
    mp3->eof = false;
    mp3->pcm_frames = 0;
    mp3->pcm_pos = 0;
    mp3dec_init(&mp3->d.dec);
    mp3->frames_base = (uint64_t)ms * mp3->info.sample_rate_hz / 1000u;
    mp3->frames_out = 0;
    return 0;
}

dmod_dmmp3_api_declaration(1.0, uint32_t, _tell, ( dmmp3_t mp3 ))
{
    if (!is_mp3(mp3) || mp3->info.sample_rate_hz == 0)
        return 0;
    return (uint32_t)((mp3->frames_base + mp3->frames_out) * 1000u / mp3->info.sample_rate_hz);
}

/* ---- The module ---- */

int dmod_init(const Dmod_Config_t* Config)
{
    (void)Config;
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}
