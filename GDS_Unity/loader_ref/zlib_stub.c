/* zlib_stub.c -- resolve libz entry points via dlsym at runtime so the loader
 * binary itself doesn't link libz.  The device ships libz.so.1; our loader
 * dlopens it RTLD_GLOBAL.  GDS's libunity imports only inflateInit2_, but the
 * reference bionic.c table lists the whole family, so provide all of them. */
#define _GNU_SOURCE
#include <stddef.h>
#include "musl_compat.h"
#include <dlfcn.h>
#include "zlib.h"

static void *zfn(const char *n) { return dlsym(RTLD_DEFAULT, n); }

int inflate(z_streamp s, int f) { static int (*fn)(z_streamp,int)=0; if(!fn) fn=zfn("inflate"); return fn?fn(s,f):0; }
int inflateInit_(z_streamp s, const char *v, int z) { static int (*fn)(z_streamp,const char*,int)=0; if(!fn) fn=zfn("inflateInit_"); return fn?fn(s,v,z):0; }
int inflateInit2_(z_streamp s, int w, const char *v, int z) { static int (*fn)(z_streamp,int,const char*,int)=0; if(!fn) fn=zfn("inflateInit2_"); return fn?fn(s,w,v,z):0; }
int inflateEnd(z_streamp s) { static int (*fn)(z_streamp)=0; if(!fn) fn=zfn("inflateEnd"); return fn?fn(s):0; }
int inflateReset(z_streamp s) { static int (*fn)(z_streamp)=0; if(!fn) fn=zfn("inflateReset"); return fn?fn(s):0; }
int inflateSetDictionary(z_streamp s, const Bytef *d, unsigned int n) { static int (*fn)(z_streamp,const Bytef*,unsigned int)=0; if(!fn) fn=zfn("inflateSetDictionary"); return fn?fn(s,d,n):0; }
int inflateCopy(z_streamp d, z_streamp s) { static int (*fn)(z_streamp,z_streamp)=0; if(!fn) fn=zfn("inflateCopy"); return fn?fn(d,s):0; }
int inflateSync(z_streamp s) { static int (*fn)(z_streamp)=0; if(!fn) fn=zfn("inflateSync"); return fn?fn(s):0; }
int deflate(z_streamp s, int f) { static int (*fn)(z_streamp,int)=0; if(!fn) fn=zfn("deflate"); return fn?fn(s,f):0; }
int deflateInit_(z_streamp s, int l, const char *v, int z) { static int (*fn)(z_streamp,int,const char*,int)=0; if(!fn) fn=zfn("deflateInit_"); return fn?fn(s,l,v,z):0; }
int deflateInit2_(z_streamp s, int l, int m, int w, int b, int n, const char *v, int z) { static int (*fn)(z_streamp,int,int,int,int,int,const char*,int)=0; if(!fn) fn=zfn("deflateInit2_"); return fn?fn(s,l,m,w,b,n,v,z):0; }
int deflateEnd(z_streamp s) { static int (*fn)(z_streamp)=0; if(!fn) fn=zfn("deflateEnd"); return fn?fn(s):0; }
int deflateReset(z_streamp s) { static int (*fn)(z_streamp)=0; if(!fn) fn=zfn("deflateReset"); return fn?fn(s):0; }
uLong deflateBound(z_streamp s, uLong n) { static uLong (*fn)(z_streamp,uLong)=0; if(!fn) fn=zfn("deflateBound"); return fn?fn(s,n):0; }
int compress(Bytef *d, uLongf *dl, const Bytef *s, uLong n) { static int (*fn)(Bytef*,uLongf*,const Bytef*,uLong)=0; if(!fn) fn=zfn("compress"); return fn?fn(d,dl,s,n):0; }
int compress2(Bytef *d, uLongf *dl, const Bytef *s, uLong n, int l) { static int (*fn)(Bytef*,uLongf*,const Bytef*,uLong,int)=0; if(!fn) fn=zfn("compress2"); return fn?fn(d,dl,s,n,l):0; }
int uncompress(Bytef *d, uLongf *dl, const Bytef *s, uLong n) { static int (*fn)(Bytef*,uLongf*,const Bytef*,uLong)=0; if(!fn) fn=zfn("uncompress"); return fn?fn(d,dl,s,n):0; }
uLong crc32(uLong c, const Bytef *b, unsigned int n) { static uLong (*fn)(uLong,const Bytef*,unsigned int)=0; if(!fn) fn=zfn("crc32"); return fn?fn(c,b,n):0; }
uLong adler32(uLong c, const Bytef *b, unsigned int n) { static uLong (*fn)(uLong,const Bytef*,unsigned int)=0; if(!fn) fn=zfn("adler32"); return fn?fn(c,b,n):0; }
const char *zlibVersion(void) { static const char *(*fn)(void)=0; if(!fn) fn=zfn("zlibVersion"); return fn?fn():"1.2.11"; }
const char *zError(int e) { static const char *(*fn)(int)=0; if(!fn) fn=zfn("zError"); return fn?fn(e):""; }
