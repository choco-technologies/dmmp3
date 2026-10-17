# dmmp3

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmmp3/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmmp3/actions/workflows/ci.yml)

MP3 files for dmod: their tags and their samples.

```c
#include "dmmp3.h"

dmmp3_info_t info;
dmmp3_read_info("/mnt/sd/music/song.mp3", &info);       /* tags, rate, channels, length - no decoder */

int status;
dmmp3_t mp3 = dmmp3_open("/mnt/sd/music/song.mp3", &status);
static int16_t pcm[1152 * 2];
int32_t frames;
while ((frames = dmmp3_read(mp3, pcm, 1152)) > 0)
    /* interleaved signed 16-bit, info.channels per frame, at info.sample_rate_hz */;
dmmp3_seek(mp3, 60000);                                   /* a minute in */
dmmp3_close(mp3);
```

- **Decoding**: [minimp3](https://github.com/lieff/minimp3) (CC0), MPEG-1,
  -2 and -2.5 layer III, constant and variable bit rates, into signed 16-bit
  samples. Its 16 kB scratch is the decoder's, not the stack's (one change of
  `src/minimp3/minimp3.h`, marked at its top), so a thread with a small stack
  decodes. A decoder takes about 44 kB of the heap: minimp3's state and
  scratch, 16 kB of the file, a frame of samples.
- **Tags**: ID3v2.2, .3 and .4 - title, artist, album, in ISO-8859-1, UTF-16
  or UTF-8, given as UTF-8 of up to 63 bytes - and ID3v1 for what ID3v2 does
  not have. Pictures and everything else are skipped, never read.
- **Covers**: `dmmp3_find_picture()` tells where the front cover (else the
  first picture) of the ID3v2 tag is in the file - the bytes of a JPEG, a
  PNG -, for an image decoder to read it from there.
- **Length**: from the Xing / Info header's frame count (VBR files made by
  LAME, ffmpeg), else from the first frame's bit rate.
- **Seeking**: by the Xing table of contents (as close as it tells), else by
  the bit rate (exact for a constant one); `dmmp3_tell()` counts the frames a
  seek leaves out until the bit reservoir is full again.

See [docs/api-reference.md](docs/api-reference.md).

## Building

```bash
cmake -S . -B build -DDMOD_TOOLS_NAME=arch/armv7/cortex-m7
cmake --build build
```

## Testing

```bash
cmake -S . -B build && cmake --build build
cd build && ctest --output-on-failure
```

`tests/fixtures` holds four small files made with ffmpeg (libmp3lame): a
constant bit rate mono file without tags, a VBR stereo file with a Xing
header and ID3v2.3 tags in UTF-16, a file with an ID3v1 tag only, a file
with a front cover (a small JPEG). The test
reads their tags and lengths, decodes them (the tones are checked by their
zero crossings) and seeks in them. The tags with pictures are made by the
test itself.

## License

MIT - see [LICENSE](LICENSE). minimp3: CC0 ([src/minimp3/LICENSE](src/minimp3/LICENSE)).
