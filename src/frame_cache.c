/*
   This file is part of harvid

   Copyright (C) 2008-2013 Robin Gareus <robin@gareus.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdint.h>     /* uint8_t */
#include <inttypes.h>
#include <stdlib.h>     /* calloc et al.*/
#include <string.h>     /* memset */
#include <unistd.h>

#include "decoder_ctrl.h"
#include "daemon_log.h"
#include "frame_cache.h"
#include "ffcompat.h"
#include "ffdecoder.h"

#include <time.h>
#include <assert.h>
#include <pthread.h>

//#define HASH_EMIT_KEYS 3
#define HASH_FUNCTION HASH_SFH
#include "uthash.h"

/* FLAGS */
#define CLF_DECODING 1 //< decoder is active
#define CLF_INUSE 2    //< currently being served
#define CLF_VALID 4    //< cacheline is valid (has decoded frame)

typedef struct videocacheline {
  int id;         // file ID from VidMap
  short w;
  short h;
  int fmt;        // pixel format
  int64_t frame;
  int flags;
  int refcnt;     // CLF_INUSE reference count
  time_t lru;     // east recently used time
  //int hitcount  //  -- unused; least-frequently used idea
  uint8_t *b;     //< data buffer pointer
  UT_hash_handle hh;
} videocacheline;

#define CLKEYLEN (offsetof(videocacheline, flags) - offsetof(videocacheline, id))

/* get a new cacheline or replace and existing one
 * NB. the cache needs to be write-locked when calling this
 * and realloccl_buf() must be called after this
 */
static videocacheline *getcl(videocacheline **cache, int cfg_cachesize,
    unsigned short id, short w, short h, int fmt, int64_t frame) {
  videocacheline *cl = NULL;

  if (HASH_COUNT(*cache) >= cfg_cachesize) {
    time_t lru = time(NULL) + 1;
    videocacheline *tmp, *clru = NULL;
    HASH_ITER(hh, *cache, cl, tmp) {
      if (cl->flags == 0) return cl;
      if (!(cl->flags&(CLF_DECODING|CLF_INUSE)) && (cl->lru < lru))  {
        lru = cl->lru;
        clru = cl;
      }
    }
    if (clru) {
      HASH_DEL(*cache, clru);
      assert(clru->refcnt == 0);
      cl = clru;
      if (cl->b && cl->w == w && cl->h == h && cl->fmt == fmt) {
        cl->flags = 0;
        memset(&cl->hh, 0, sizeof(UT_hash_handle));
      } else {
        free(cl->b);
        memset(cl, 0, sizeof(videocacheline));
      }
    } else {
      dlog(DLOG_WARNING, "CACHE: cache full - all cache-lines in use.\n");
      return NULL;
    }
  }

  if (!cl)
    cl = calloc(1, sizeof(videocacheline));

  cl->id = id;
  cl->w = w;
  cl->h = h;
  cl->fmt = fmt;
  cl->frame = frame;
  cl->lru = 0;
  HASH_ADD(hh, *cache, id, CLKEYLEN, cl);
  return cl;
}

/* check if requested data exists in cache */
static videocacheline *testclwh(videocacheline *cache,
    pthread_rwlock_t *lock,
    int64_t frame, short w, short h, int fmt, unsigned short id) {
  videocacheline *rv;
  const videocacheline cmp = {id, w, h, fmt, frame, 0, 0, 0, NULL };
  pthread_rwlock_rdlock(lock);
  HASH_FIND(hh, cache, &cmp, CLKEYLEN, rv);
  pthread_rwlock_unlock(lock);
  return rv;
}

/* clear cache
 * if f==1 wait for used cachelines to become unused
 * if f==0 the cache is flushed objects in use are retained
 * time a cacheline is needed
 */
static void clearcache(videocacheline **cache, pthread_rwlock_t *cachelock, int f, int id) {
  videocacheline *tmp, *cl = NULL;
  HASH_ITER(hh, *cache, cl, tmp) {
    if (id >= 0 && cl->id != id) {
      continue;
    }
    if (f) {
      if (cl->flags & (CLF_DECODING|CLF_INUSE)) {
        dlog(DLOG_WARNING, "CACHE: waiting for cacheline to be unlocked.\n");
      }
      while (cl->flags & (CLF_DECODING|CLF_INUSE)) {
        pthread_rwlock_unlock(cachelock);
        mymsleep(5);
        pthread_rwlock_wrlock(cachelock);
      }
    }
    if (cl->flags & (CLF_DECODING|CLF_INUSE)) {
      continue;
    }
    HASH_DEL(*cache, cl);
    assert(cl->refcnt == 0);
    free(cl->b);
    free(cl);
  }
}

static void realloccl_buf(videocacheline *cptr, int w, int h, int fmt) {
  if (cptr->b && cptr->w == w && cptr->h == h && cptr->fmt == fmt)
    return; // already allocated

  free(cptr->b);
  cptr->b = calloc(picture_bytesize(fmt, w, h), sizeof(uint8_t));
}

///////////////////////////////////////////////////////////////////////////////
// Cache Control

typedef struct {
  int cfg_cachesize;
  videocacheline *vcache;
  pthread_rwlock_t lock;
  int cache_hits;
  int cache_miss;
} xjcd;

static void fc_initialize_cache (xjcd *cc) {
  assert(!cc->vcache);
  cc->vcache = NULL;
  cc->cache_hits = 0;
  cc->cache_miss = 0;
  pthread_rwlock_init(&cc->lock, NULL);
}

static void fc_flush_cache (xjcd *cc) {
  pthread_rwlock_wrlock(&cc->lock);
  clearcache(&cc->vcache, &cc->lock, 1, -1);
  cc->cache_hits = 0;
  cc->cache_miss = 0;
  pthread_rwlock_unlock(&cc->lock);
}

static videocacheline *fc_readcl(xjcd *cc, void *dc, int64_t frame, short w, short h, int fmt, unsigned short vid) {
  /* check if the requested frame is cached */
  videocacheline *rv = testclwh(cc->vcache, &cc->lock, frame, w, h, fmt, vid);
  if (rv) {
    pthread_rwlock_wrlock(&cc->lock); // rdlock should suffice here
    /* check if it has been recently invalidated by another thread */
    if (rv->flags&CLF_VALID) {
      rv->refcnt++;
      rv->flags |= CLF_INUSE;
      pthread_rwlock_unlock(&cc->lock);
      rv->lru = time(NULL);
      cc->cache_hits++;
      return(rv);
    }
    pthread_rwlock_unlock(&cc->lock);
    rv = NULL;
  }

  /* too bad, now we need to allocate a new or free an used
   * cacheline and then decode the video... */
  int timeout = 250; /* 1 second to get a buffer */
  do {
    pthread_rwlock_wrlock(&cc->lock);
    rv = getcl(&cc->vcache, cc->cfg_cachesize, vid, w, h, fmt, frame);
    if (rv) {
      rv->flags |= CLF_DECODING;
    }
    pthread_rwlock_unlock(&cc->lock);
    if (!rv) {
      mymsleep(5);
    }
  } while(--timeout > 0 && !rv);

  if (!rv) {
    /* no buffer available */
    return NULL;
  }

  /* set w,h,fmt and re-alloc buffer if neccesary */
  realloccl_buf(rv, w, h, fmt);

  /* fill cacheline with data - decode video */
  if (dctrl_decode(dc, vid, frame, rv->b, w, h, fmt)) {
    /* we don't cache decode-errors */
    pthread_rwlock_wrlock(&cc->lock); // rdlock should suffice here
    rv->flags &= ~CLF_VALID;
    rv->flags &= ~CLF_DECODING;
    rv->flags |= CLF_INUSE;
    rv->refcnt++;
    pthread_rwlock_unlock(&cc->lock);
    dlog(DLOG_WARNING, "CACHE: decode failed.\n");
    return (rv); // this is OK, a black frame will have been rendered
  }

  rv->lru = time(NULL);
  pthread_rwlock_wrlock(&cc->lock); // rdlock should suffice here
  rv->flags |= CLF_VALID|CLF_INUSE;
  rv->flags &= ~CLF_DECODING;
  rv->refcnt++;
  pthread_rwlock_unlock(&cc->lock);
  cc->cache_miss++;
  return(rv);
}

///////////////////////////////////////////////////////////////////////////////
// public API

void vcache_clear (void *p, int id) {
  xjcd *cc = (xjcd*) p;
  pthread_rwlock_wrlock(&cc->lock);
  clearcache(&cc->vcache, &cc->lock, 0, id);
  cc->cache_hits = 0;
  cc->cache_miss = 0;
  pthread_rwlock_unlock(&cc->lock);
}

void vcache_create(void **p) {
  (*((xjcd**)p)) = (xjcd*) calloc(1, sizeof(xjcd));
  (*((xjcd**)p))->cfg_cachesize = 48;
  fc_initialize_cache((*((xjcd**)p)));
}

void vcache_resize(void **p, int size) {
  if (size < (*((xjcd**)p))->cfg_cachesize)
    fc_flush_cache((*((xjcd**)p)));
  if (size > 0)
    (*((xjcd**)p))->cfg_cachesize = size;
}

void vcache_destroy(void **p) {
  xjcd *cc = *(xjcd**) p;
  fc_flush_cache(cc);
  pthread_rwlock_destroy(&cc->lock);
  free(cc->vcache);
  free(cc);
  *p = NULL;
}

uint8_t *vcache_get_buffer(void *p, void *dc, unsigned short id, int64_t frame, short w, short h, int fmt, void **cptr) {
  videocacheline *cl = fc_readcl((xjcd*)p, dc, frame, w, h, fmt, id);
  if (!cl) {
    if (cptr) *cptr = NULL;
    return NULL;
  }
  if (cptr) *cptr = cl;
  return cl->b;
}

void vcache_release_buffer(void *p, void *cptr) {
  xjcd *cc = (xjcd*) p;
  videocacheline *cl = (videocacheline *)cptr;
  if (!cptr) return;
  pthread_rwlock_wrlock(&cc->lock);
  if (--cl->refcnt < 1) {
    assert(cl->refcnt >= 0);
    cl->flags &= ~CLF_INUSE;
  }
  // TODO relete cacheline IFF !CLF_VALID (decode failed)
  pthread_rwlock_unlock(&cc->lock);
}

///////////////////////////////////////////////////////////////////////////////
// statistics

static char *flags2txt(int f) {
  char *rv = NULL;
  size_t off = 0;

  if (f == 0) {
    rv = (char*) realloc(rv, (off+2) * sizeof(char));
    off += sprintf(rv+off, "-");
    return rv;
  }
  if (f&CLF_DECODING) {
    rv = (char*) realloc(rv, (off+10) * sizeof(char));
    off += sprintf(rv+off, "decoding ");
  }
  if (f&CLF_VALID) {
    rv = (char*) realloc(rv, (off+7) * sizeof(char));
    off += sprintf(rv+off, "valid ");
  }
  if (f&CLF_INUSE) {
    rv = (char*) realloc(rv, (off+8) * sizeof(char));
    off += sprintf(rv+off, "in-use ");
  }
  return rv;
}

void vcache_info_html(void *p, char **m, size_t *o, size_t *s) {
  int i = 1;
  videocacheline *cptr, *tmp;

  rprintf("<h3>Cache Info:</h3>\n");
  rprintf("<p>Size: max. %i entries.\n", ((xjcd*)p)->cfg_cachesize);
  rprintf("Hits: %d, Misses: %d</p>\n", ((xjcd*)p)->cache_hits, ((xjcd*)p)->cache_miss);
  rprintf("<table style=\"text-align:center;width:100%%\">\n");
  rprintf("<tr><th>#</th><th>file-id</th><th>Flags</th><th>W</th><th>H</th><th>Buffer</th><th>Frame#</th><th>LRU</th></tr>\n");
  /* walk comlete tree */
  pthread_rwlock_rdlock(&((xjcd*)p)->lock);
  HASH_ITER(hh, ((xjcd*)p)->vcache, cptr, tmp) {
    char *tmp = flags2txt(cptr->flags);
    rprintf(
        "<tr><td>%d.</td><td>%d</td><td>%s</td><td>%d</td><td>%d</td><td>%s</td><td>%"PRId64"</td><td>%"PRIlld"</td></tr>\n",
        i, cptr->id, tmp, cptr->w, cptr->h, (cptr->b ? ff_fmt_to_text(cptr->fmt) : "null"), cptr->frame, (long long) cptr->lru);
    free(tmp);
    i++;
  }
  pthread_rwlock_unlock(&((xjcd*)p)->lock);
  rprintf("</table>\n");
}

// vim:sw=2 sts=2 ts=8 et:
