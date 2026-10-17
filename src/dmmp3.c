#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmmp3.h"
#include <errno.h>
#include <string.h>

/*
 * The decoder: minimp3 fed from a buffer of the file. Its scratch - 16 kB
 * that minimp3 keeps on the stack - is the decoder's (MINIMP3_SCRATCH), so
 * a thread with a small stack can decode. minimp3's state and the scratch
 * are worked on all the time: they are a block of their own (state_t), of
 * "fast" memory when the system has it (Dmod_MallocTagged) - internal RAM,
 * where the default memory is external behind a small cache. A closed
 * decoder's block is kept for the next one (g_spare): a player going from
 * file to file keeps its fast memory - freed and allocated again, the place
 * could be taken by something small meanwhile, and the next one not fit.
 */

static void* scratch_of(void* dec);
#define MINIMP3_SCRATCH(dec)    scratch_of(dec)
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3        /* Layer III only: what MP3 files are */
#include "minimp3/minimp3.h"

#define BUFFER_SIZE     16384u      /* Of the file at once: minimp3 syncs on several frames */
#define MAX_TAG_FRAME   4096u       /* A tag text longer than this is not read (pictures are skipped) */
#define PICTURE_HEAD    512u        /* Of a picture frame read for its header: the picture's description fits in it */
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

/* What decoding works on all the time */
typedef struct
{
    decoder_t           d;
    mp3dec_scratch_t    scratch;
} state_t;

struct dmmp3
{
    uint32_t            magic;
    state_t*            state;          /* "fast" memory, when there is (Dmod_MallocTagged) */
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

/* An ID3v2 tag at the start of a file: its frames, one after another */
typedef struct
{
    void*       f;
    uint8_t     version;        /* 2, 3, 4 */
    bool        unsync;         /* The whole tag unsynchronised */
    uint32_t    size;           /* Of the frames, with the padding */
    uint32_t    at;             /* The next frame */
    uint32_t    end;            /* Of the tag: where the audio may start */
} id3_t;

/* A frame of it */
typedef struct
{
    char        id[5];
    uint32_t    at;             /* Of its data */
    uint32_t    size;
    uint8_t     flags;          /* Its format flags (2.3, 2.4) */
} frame_t;

/* The tag's header; false when the file has no ID3v2 tag */
static bool id3_open(void* f, id3_t* t)
{
    uint8_t h[10];
    if (!read_at(f, 0, h, sizeof(h)) || memcmp(h, "ID3", 3) != 0 || h[3] < 2 || h[3] > 4)
        return false;
    t->f = f;
    t->version = h[3];
    t->unsync = (h[5] & 0x80u) != 0;
    t->size = syncsafe(h + 6);
    t->end = 10u + t->size + (((h[5] & 0x10u) != 0) ? 10u : 0u);      /* (with a footer) */
    t->at = 10;
    if (t->version >= 3 && (h[5] & 0x40u) != 0)
    {
        /* An extended header: skipped */
        uint8_t e[4];
        if (!read_at(f, t->at, e, 4))
            t->at = t->end;
        else
            t->at += (t->version == 4) ? syncsafe(e) : be32(e) + 4u;
    }
    return true;
}

/* The next frame; false after the last one (the padding, the end of the tag) */
static bool id3_next(id3_t* t, frame_t* fr)
{
    uint32_t header = (t->version == 2) ? 6u : 10u;
    uint8_t fh[10];
    if (t->at + header > 10u + t->size || !read_at(t->f, t->at, fh, header) || fh[0] == 0)
        return false;                                   /* The padding */
    memset(fr, 0, sizeof(*fr));
    if (t->version == 2)
    {
        memcpy(fr->id, fh, 3);
        fr->size = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
    }
    else
    {
        memcpy(fr->id, fh, 4);
        fr->size = (t->version == 4) ? syncsafe(fh + 4) : be32(fh + 4);
        fr->flags = fh[9];
    }
    fr->at = t->at + header;
    if (fr->size > t->size)
        return false;
    t->at = fr->at + fr->size;
    return true;
}

/* ID3v2 at the start: its tags; returns the bytes it takes (0: none) */
static uint32_t read_id3v2(void* f, dmmp3_info_t* info)
{
    id3_t t;
    frame_t fr;
    if (!id3_open(f, &t))
        return 0;
    uint8_t* text = Dmod_Malloc(MAX_TAG_FRAME);
    while (text != NULL && id3_next(&t, &fr))
    {
        char* to = (strcmp(fr.id, "TIT2") == 0 || strcmp(fr.id, "TT2") == 0) ? info->title :
                   (strcmp(fr.id, "TPE1") == 0 || strcmp(fr.id, "TP1") == 0) ? info->artist :
                   (strcmp(fr.id, "TALB") == 0 || strcmp(fr.id, "TAL") == 0) ? info->album : NULL;
        if (to != NULL && fr.size > 0 && fr.size <= MAX_TAG_FRAME && read_at(f, fr.at, text, fr.size))
            tag_text(text, fr.size, to);
        /* (a picture: skipped, never read) */
    }
    if (text != NULL)
        Dmod_Free(text);
    return t.end;
}

/*
 * A picture frame (APIC, 2.2: PIC) whose image is in the file as it is: not
 * unsynchronised, compressed or encrypted. Its image: after the header of the
 * frame's data - the text encoding, the MIME type (2.2: a 3-letter format),
 * the picture type and a description. False: not such a frame.
 */
static bool picture_of(const id3_t* t, const frame_t* fr, uint8_t* head, dmmp3_picture_t* picture)
{
    uint32_t skip = 0;                                  /* Before the data: a group, a length */
    if (t->unsync)
        return false;
    if (t->version == 2)
    {
        if (strcmp(fr->id, "PIC") != 0)
            return false;
    }
    else if (strcmp(fr->id, "APIC") != 0)
        return false;
    else if (t->version == 3)
    {
        if ((fr->flags & 0xC0u) != 0)                  /* Compressed, encrypted */
            return false;
        skip = ((fr->flags & 0x20u) != 0) ? 1u : 0u;
    }
    else
    {
        if ((fr->flags & 0x0Eu) != 0)                  /* Compressed, encrypted, unsynchronised */
            return false;
        skip = (((fr->flags & 0x40u) != 0) ? 1u : 0u) + (((fr->flags & 0x01u) != 0) ? 4u : 0u);
    }
    if (fr->size <= skip)
        return false;
    uint32_t n = fr->size - skip;
    if (n > PICTURE_HEAD)
        n = PICTURE_HEAD;
    if (!read_at(t->f, fr->at + skip, head, n))
        return false;
    uint32_t i = 1;                                     /* After the encoding */
    if (t->version == 2)
        i += 3;
    else
    {
        while (i < n && head[i] != 0)
            i++;
        i++;
    }
    if (i >= n)
        return false;
    uint8_t type = head[i++];
    bool wide = head[0] == 1 || head[0] == 2;         /* UTF-16: ended by two zeros, at an even place */
    uint32_t start = i;
    while (i < n && !(wide ? (i + 1U < n && head[i] == 0 && head[i + 1U] == 0 && ((i - start) & 1u) == 0) : head[i] == 0))
        i++;
    i += wide ? 2u : 1u;
    if (i >= n)
        return false;                                   /* (a description longer than PICTURE_HEAD: not read) */
    picture->offset = fr->at + skip + i;
    picture->size = fr->size - skip - i;
    picture->type = type;
    return true;
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

dmod_dmmp3_api_declaration(1.0, int, _find_picture, ( const char* path, dmmp3_picture_t* picture ))
{
    if (path == NULL || picture == NULL)
        return -EINVAL;
    void* f = Dmod_FileOpen(path, "rb");
    if (f == NULL)
        return -ENOENT;
    id3_t t;
    frame_t fr;
    dmmp3_picture_t p;
    bool found = false;
    uint8_t* head = Dmod_Malloc(PICTURE_HEAD);
    int ret = (head == NULL) ? -ENOMEM : -ENODATA;
    if (head != NULL && id3_open(f, &t))
    {
        while (id3_next(&t, &fr))
        {
            if (!picture_of(&t, &fr, head, &p) || p.size == 0 || (found && p.type != DMMP3_PICTURE_FRONT))
                continue;
            *picture = p;                               /* The first one, or the front cover */
            found = true;
            ret = 0;
            if (p.type == DMMP3_PICTURE_FRONT)
                break;
        }
    }
    if (head != NULL)
        Dmod_Free(head);
    Dmod_FileClose(f);
    return ret;
}

/* ---- Decoding ---- */

static state_t* g_spare;                /* A closed decoder's state, for the next one */

static state_t* state_new(void)
{
    Dmod_EnterCritical();
    state_t* s = g_spare;
    g_spare = NULL;
    Dmod_ExitCritical();
    return (s != NULL) ? s : Dmod_MallocTagged("fast", sizeof(state_t));
}

static void state_free(state_t* s)
{
    Dmod_EnterCritical();
    bool kept = g_spare == NULL;
    if (kept)
        g_spare = s;
    Dmod_ExitCritical();
    if (!kept)
        Dmod_Free(s);
}

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
    if ((m->state = state_new()) == NULL)
    {
        Dmod_FileClose(f);
        Dmod_Free(m);
        *status = -ENOMEM;
        return NULL;
    }
    m->magic = MP3_MAGIC;
    m->file = f;
    m->state->d.scratch = &m->state->scratch;
    mp3dec_init(&m->state->d.dec);
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
    state_free(mp3->state);
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
        int samples = mp3dec_decode_frame(&m->state->d.dec, m->buffer + m->pos, (int)(m->length - m->pos), m->pcm, &fi);
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
    mp3dec_init(&mp3->state->d.dec);
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
    if (g_spare != NULL)
        Dmod_Free(g_spare);
    g_spare = NULL;
    return 0;
}
