#ifndef DMMP3_H
#define DMMP3_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmmp3_defs.h"

/**
 * dmmp3 - MP3 files: their tags and their samples.
 *
 * A file is opened (dmmp3_open()) and read as interleaved signed 16-bit
 * samples (dmmp3_read()), in the rate and channels of the stream; it can be
 * moved in (dmmp3_seek()). dmmp3_read_info() reads only what a list of files
 * needs - the tags and the length - without a decoder.
 *
 * The decoding is minimp3's (CC0, https://github.com/lieff/minimp3): MPEG-1,
 * -2, -2.5 layer III (and I, II), constant and variable bit rates. Tags:
 * ID3v2.2, .3, .4 (title, artist, album; ISO-8859-1, UTF-16, UTF-8 texts,
 * given as UTF-8) and ID3v1. The length is the Xing / Info header's frame
 * count, else estimated from the bit rate of the first frame.
 *
 * A file's cover (dmmp3_find_picture()) is not read: what is given is where
 * the image file (a JPEG, a PNG ...) is in the MP3 file, for an image decoder
 * to read it from there.
 */

/** Bytes of a tag text with its terminating zero (longer ones are cut, not in a character). */
#define DMMP3_TAG_SIZE      64u

typedef struct dmmp3* dmmp3_t;

/** A file: its stream and its tags. */
typedef struct
{
    uint32_t    sample_rate_hz;
    uint8_t     channels;                   /**< 1 or 2 */
    uint16_t    bitrate_kbps;               /**< Of the first frame; the average of a file with a Xing header */
    bool        vbr;                        /**< A Xing header says the bit rate varies */
    uint32_t    duration_ms;                /**< 0: not known */
    char        title[DMMP3_TAG_SIZE];      /**< UTF-8, "" without a tag */
    char        artist[DMMP3_TAG_SIZE];
    char        album[DMMP3_TAG_SIZE];
} dmmp3_info_t;

/** ID3v2's picture type of the front cover */
#define DMMP3_PICTURE_FRONT 3u

/** A picture of a file's ID3v2 tag (an APIC frame, 2.2: PIC): where its image is. */
typedef struct
{
    uint32_t    offset;                     /**< Of the image's first byte in the MP3 file */
    uint32_t    size;                       /**< Of the image, bytes */
    uint8_t     type;                       /**< ID3v2's picture type: DMMP3_PICTURE_FRONT, 0 other, ... */
} dmmp3_picture_t;

/**
 * @brief Open an MP3 file for decoding.
 *
 * @param status Receives 0, -ENOENT, -EBADMSG (no MP3 frame in it), -ENOMEM (may be NULL)
 * @return The decoder, NULL on failure
 */
dmod_dmmp3_api(1.0, dmmp3_t, _open, ( const char* path, int* status ));

/** @brief Close it. Safe on NULL. */
dmod_dmmp3_api(1.0, void, _close, ( dmmp3_t mp3 ));

/** @brief Its stream and tags. @return 0, -EINVAL */
dmod_dmmp3_api(1.0, int, _get_info, ( dmmp3_t mp3, dmmp3_info_t* info ));

/**
 * @brief Decode the next @p frames frames (one sample per channel each,
 *        interleaved) into @p pcm.
 *
 * @return Frames decoded - fewer only at the end of the file, 0 there -,
 *         -EIO, -EINVAL
 */
dmod_dmmp3_api(1.0, int32_t, _read, ( dmmp3_t mp3, int16_t* pcm, uint32_t frames ));

/**
 * @brief Go to @p ms milliseconds from the start: by the Xing table of
 *        contents, else by the bit rate (exact for a constant one).
 * @return 0, -EINVAL, -EIO
 */
dmod_dmmp3_api(1.0, int, _seek, ( dmmp3_t mp3, uint32_t ms ));

/** @brief Milliseconds of the samples read so far (from the start, or from where a seek put it). */
dmod_dmmp3_api(1.0, uint32_t, _tell, ( dmmp3_t mp3 ));

/**
 * @brief The tags and the stream of a file, without opening a decoder (for a
 *        list of files: a few small reads).
 * @return 0, -ENOENT, -EBADMSG (no MP3 frame), -EINVAL
 */
dmod_dmmp3_api(1.0, int, _read_info, ( const char* path, dmmp3_info_t* info ));

/**
 * @brief Where the cover of a file is: the front cover of its ID3v2 tag, else
 *        its first picture. Only a picture stored as it is - not
 *        unsynchronised, compressed or encrypted - is given: the image is
 *        read from the file at `offset`.
 * @return 0, -ENOENT (no file), -ENODATA (no such picture), -ENOMEM, -EINVAL
 */
dmod_dmmp3_api(1.0, int, _find_picture, ( const char* path, dmmp3_picture_t* picture ));

#endif /* DMMP3_H */
