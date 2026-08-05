#ifndef ZLIB_H
#define ZLIB_H
#include <stddef.h>
typedef unsigned long uLong;
typedef unsigned long uLongf;
typedef unsigned int uInt;
typedef unsigned char Bytef;
typedef struct z_stream_s { unsigned char *next_in; unsigned int avail_in; unsigned long total_in; unsigned char *next_out; unsigned int avail_out; unsigned long total_out; void *msg; void *state; void *zalloc; void *zfree; void *opaque; int data_type; unsigned long adler; unsigned long reserved; } z_stream;
typedef z_stream *z_streamp;
#define Z_NO_FLUSH 0
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_STREAM_ERROR (-2)
extern int inflate(z_streamp, int);
extern int inflateInit_(z_streamp, const char *, int);
extern int inflateInit2_(z_streamp, int, const char *, int);
extern int inflateEnd(z_streamp);
extern int inflateReset(z_streamp);
extern int inflateSetDictionary(z_streamp, const Bytef *, unsigned int);
extern int inflateCopy(z_streamp, z_streamp);
extern int inflateSync(z_streamp);
extern int deflate(z_streamp, int);
extern int deflateInit_(z_streamp, int, const char *, int);
extern int deflateInit2_(z_streamp, int, int, int, int, int, const char *, int);
extern int deflateEnd(z_streamp);
extern int deflateReset(z_streamp);
extern uLong deflateBound(z_streamp, uLong);
extern int compress(Bytef *, uLongf *, const Bytef *, uLong);
extern int compress2(Bytef *, uLongf *, const Bytef *, uLong, int);
extern int uncompress(Bytef *, uLongf *, const Bytef *, uLong);
extern uLong crc32(uLong, const Bytef *, unsigned int);
extern uLong adler32(uLong, const Bytef *, unsigned int);
extern const char *zlibVersion(void);
extern const char *zError(int);
#endif
