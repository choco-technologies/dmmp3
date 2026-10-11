# dmmp3 API Reference

```c
#include "dmmp3.h"
```

Errors are negative `errno` values.

## `dmmp3_info_t`

| Field | |
|-------|-|
| `sample_rate_hz`, `channels` | Of the stream (its first frame); `channels` 1 or 2 |
| `bitrate_kbps` | Of the first frame; with a Xing header the average |
| `vbr` | A Xing header (not Info) says the bit rate varies |
| `duration_ms` | From the Xing frame count, else the bit rate; 0 when not known |
| `title`, `artist`, `album` | UTF-8, cut to `DMMP3_TAG_SIZE` (64) with the zero, not in a character; `""` without a tag |

## Functions

| Function | |
|----------|-|
| `int dmmp3_read_info(const char* path, dmmp3_info_t* info)` | The tags and the stream without a decoder - for a list of files. `-ENOENT`, `-EBADMSG` (no MP3 frame) |
| `dmmp3_t dmmp3_open(const char* path, int* status)` | A decoder of the file; `status` 0, `-ENOENT`, `-EBADMSG`, `-ENOMEM` |
| `int dmmp3_get_info(dmmp3_t mp3, dmmp3_info_t* info)` | What `dmmp3_read_info()` gives |
| `int32_t dmmp3_read(dmmp3_t mp3, int16_t* pcm, uint32_t frames)` | Up to `frames` frames (`channels` interleaved samples each); fewer only at the end, 0 there; `-EIO` |
| `int dmmp3_seek(dmmp3_t mp3, uint32_t ms)` | To `ms` from the start (clamped to the length) - see below |
| `uint32_t dmmp3_tell(dmmp3_t mp3)` | Milliseconds of what was read, from the start or the last seek's position |
| `void dmmp3_close(dmmp3_t mp3)` | Safe on NULL |

A seek goes by the Xing table of contents - a byte position for every
percent of the time, as precise as the encoder made it - or, without one,
by the bit rate. minimp3 then leaves out the frames that need the bit
reservoir of the frames before them (a few); their time is counted by
`dmmp3_tell()`.
