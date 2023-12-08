#define _GNU_SOURCE

#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/mman.h>

#include <immintrin.h>

#include "file_io.h"
#include "args.h"

// FILE_STAT_COUNT is somewhat mis-named, it indirectly represents the
// maximum number of open files we can have in flight. As files are opened
// first and then sent, there is potential to have all slots filled with
// open files. As Linux has a soft limit of 1024 files, we set this number
// to something below that maximum. It must always be set below the configured
// FD limit.

// The AVX routines are sort of stand-ins for atomically do something with
// a cacheline of memory. Should be replaced with something less platform
// dependant.
void memcpy_avx( void* dst, void* src ) {
          __m512i a;

          // b = _mm512_load_epi64 ( (void*) (((uint64_t) src) +  64) );
          a = _mm512_load_epi64 ( src );
          // _mm512_store_epi64( (void*) (((uint64_t) src) +  64), b );
          _mm512_store_epi64( dst, a );

}

void memset_avx( void* dst ) {
          __m512i a = {0};
          _mm512_store_epi64( dst, a );
}

// Soft limit on file descriptors, must at least 50 less than FD limit, and
// much less than HSZ (which must be ^2 aligned).
#define FILE_STAT_COUNT 800
#define FILE_STAT_COUNT_HSZ 4096

#define FS_INIT        0xBAEBEEUL
#define FS_IO          (1UL << 31)
#define FS_COMPLETE    (1UL << 30)

struct file_stat_type file_stat[FILE_STAT_COUNT_HSZ]={0};

uint64_t file_count __attribute__ ((aligned(64)));
uint64_t file_head  __attribute__ ((aligned(64)));
uint64_t file_tail  __attribute__ ((aligned(64)));

struct file_stat_type file_activefile[THREAD_COUNT] = {0};

static inline uint64_t xorshift64s(uint64_t* x) {
  *x ^= *x >> 12; // a
  *x ^= *x << 25; // b
  *x ^= *x >> 27; // c
  return *x * 0x2545F4914F6CDD1DUL;
}

struct file_stat_type* file_addfile( uint64_t fileno, int fd, uint32_t crc,
                                     int64_t file_sz ) {
  struct file_stat_type fs = {0};
  int i;

  uint64_t slot = fileno;
  slot = xorshift64s(&slot);

  uint64_t fc, ft;


  while (1) {
    // If Queue full, idle
    fc = __sync_fetch_and_add( &file_count, 0);
    ft = __sync_fetch_and_add( &file_tail, 0);

    if ( (fc-ft) < FILE_STAT_COUNT )
      break;

    usleep(10000);
  }

  for (i=0; i<4; i++) {
    slot &= FILE_STAT_COUNT_HSZ-1;
    if ( __sync_val_compare_and_swap( &file_stat[slot].state, 0, 0xBedFaceUL ) ) {
      slot = xorshift64s(&slot);
      continue;
    }
    break;
  }

  VRFY( i<4, "Hash table collision count exceeded. Please report this error.");

  fs.state = FS_INIT;
  fs.fd = fd;
  fs.file_no = fileno;
  fs.bytes = file_sz;
  fs.position = slot;
  fs.poison = 0xC0DAB1E;

  memcpy_avx( &file_stat[slot], &fs );
  __sync_fetch_and_add( &file_count, 1 );

  DBG("file_addfile fn=%ld, fd=%d crc=%X slot=%ld cc=%d",
      fileno, fd, crc, slot, i);

  return( &file_stat[slot] );
}

struct file_stat_type* file_wait( uint64_t fileno, struct file_stat_type* test_fs ) {

  int i;
  uint64_t fc, slot;

  DBG("file_wait start fn=%ld", fileno);

  while (1) {
    fc = __sync_fetch_and_add( &file_count, 0);
    if (fileno <= fc) {
      break;;
    }
    usleep (100);
  }

  DBG("file_wait ready on fn=%ld", fileno);

  slot = fileno;

  for (i=0; i<4; i++) {
    slot = xorshift64s(&slot);
    slot &= FILE_STAT_COUNT_HSZ-1;

    memcpy_avx( test_fs, &file_stat[slot] );
    if (test_fs->file_no != fileno)
      continue;

    if ( (test_fs->state == FS_INIT) && (__sync_val_compare_and_swap(
      &file_stat[slot].state, FS_INIT, FS_COMPLETE|FS_IO) == FS_INIT ) )
    {
      DBG("NEW IOW on fn=%ld, slot=%d", test_fs->file_no, i);
      return &file_stat[slot]; // Fist worker on file
    }

    if (test_fs->state & FS_IO) {
      DBG("ADD writer to fn=%ld", test_fs->file_no);
      return &file_stat[slot]; // Add worker to file
    }
  }

  VRFY(0, "Couldn't convert fileno");

}


struct file_stat_type* file_next( int id, struct file_stat_type* test_fs ) {

  // Generic function to fetch the next file, may return FD to multiple
  // threads depending on work load / incomming file stream.
  //
  DBV("[%2d] Enter file_next", id);

  uint64_t fc,fh,slot;
  int i,j;

  while (1) {
    fc = __sync_fetch_and_add( &file_count, 0);
    fh = __sync_fetch_and_add( &file_head, 0);

    // First try to attach to a new file
    if ( (fc-fh) > THREAD_COUNT ) {
      fh = __sync_fetch_and_add( &file_head, 1 );
      break;
    }

    if ( (fc-fh) > 0 ) {
      if ( __sync_val_compare_and_swap( &file_head, fh, fh+1) == fh)
        break;
      continue;
    }

    // No new files, see if we can attach to an existing file
    for (i=0; i< THREAD_COUNT; i++) {
      // Should iterate on actual thread count instead of max threads
      j = __sync_fetch_and_add( &file_activefile[i].position, 0 );
      if (j) {
        uint64_t st;
        memcpy_avx( test_fs, &file_activefile[j] );
        st = test_fs->state;
        if ( (test_fs->state & FS_IO) && (__sync_val_compare_and_swap(
          &file_stat[test_fs->position].state, st, st| (1<<id) ) == st) ) {
          return &file_stat[test_fs->position]; // Fist worker on file
        }
      }
    }

    // Got nothing, wait and try again later.
    usleep(10000);
  }

  // We got a file_no, now we need to translate it into a slot.

  slot = ++fh;

  for (i=0; i<4; i++) {
    slot = xorshift64s(&slot);
    slot &= FILE_STAT_COUNT_HSZ-1;

    memcpy_avx( test_fs, &file_stat[slot] );
    if (test_fs->file_no != fh) {
      NFO("[%2d] Failed to convert fn=%ld, slot=%ld, fh=%ld", id, test_fs->file_no, slot, fh);
      continue;
    }

    // XXX: Populate file_activefile

    if ( (test_fs->state == FS_INIT) && (__sync_val_compare_and_swap(
      &file_stat[slot].state, FS_INIT, FS_COMPLETE|FS_IO|(1<<id) ) == FS_INIT))
    {
      DBG("[%2d] NEW IOW on fn=%ld, slot=%ld", id, test_fs->file_no, slot);
      return &file_stat[slot]; // Fist worker on file
    } else {
      NFO("[%2d] Failrd to convert fn=%ld, slot=%ld", id, test_fs->file_no, slot);
    }
  }

  VRFY( 0, "[%2d] Error claiming file fn=%ld", id, fh );

}

uint64_t  file_iow_remove( struct file_stat_type* fs, int id ) {
  DBV("[%2d] Release interest in file: fn=%ld state=%lX fd=%d", id, fs->file_no, fs->state, fs->fd);

  uint64_t res = __sync_and_and_fetch( &fs->state, ~((1UL << id) | FS_IO) );

  if (res == FS_COMPLETE) {
    __sync_fetch_and_add( &file_tail, 1 );
  }

  return res;
}


int file_get_activeport( void* args_raw ) {
  int res;
  struct dtn_args* args = (struct dtn_args*) args_raw;

  while ( !(res= __sync_add_and_fetch(&args->active_port, 0) ) ) {
    usleep(1000);
  }

  return res;
}


int32_t file_hash( void* block, int sz, int seed ) {
  uint32_t *block_ptr = (uint32_t*) block;
  uint64_t hash = seed;
  hash = xorshift64s(&hash);
  int i=0;

  if (sz%4) {
    for ( i=0; i < (4-(sz%4)); i++ ) {
      ((uint8_t*)block_ptr)[sz+i] = 0;
    }
  }

  for( i=0; i<(sz+3)/4; i++ ) {
    hash = __builtin_ia32_crc32si( block_ptr[i], hash );
  }

  return hash;
}

struct file_object* file_memoryinit( void* arg, int id ) {
  struct dtn_args* dtn = arg;
  struct file_object* fob = aligned_alloc( 64, sizeof(struct file_object) );
  struct file_object f = {0};

  memset( fob, 0, sizeof(struct file_object) );

  f.io_type = dtn->io_engine;
  f.QD = dtn->QD;
  f.blk_sz = dtn->block;
  f.io_flags = dtn->flags;
  f.thread_count = dtn->thread_count;
  f.args = &dtn->io_engine_name[5];
  f.id = id;

  switch (f.io_type) {
#ifdef __ENGINE_POSIX__
    case FIIO_POSIX:
      file_posixinit( &f );
      break;
#endif
#ifdef __ENGINE_URING__
    case FIIO_URING:
      file_uringinit( &f );
      break;
#endif
#ifdef __ENGINE_DUMMY__
    case FIIO_DUMMY:
      file_dummyinit( &f );
      break;
#endif
/*
    case FIIO_SHMEM:
      shmem_init( &f );
      break;
*/
    default:
      VRFY( 0, "No matching engine for '%x'",
               fob->io_type );
  }

  memcpy ( fob, &f, sizeof(struct file_object) );

  return fob;

}

void file_prng( void* buf, int sz ) {
  int i=0, offset=0;
  uint64_t* b = buf;
  static __thread uint64_t s=0;

  if ( s == 0 )
    file_randrd( &s, 8 );

  while ( sz > 1023 ) {
    // Compiler optimization hint
    for (i=0; i<128; i++)
      b[offset+i] = xorshift64s(&s);
    offset += 128;
    sz -= 1024;
  }

  for ( i=0; i < ((sz+7)/8); i++ ) {
      b[offset+i] = xorshift64s(&s);
  }
}

void file_randrd( void* buf, int count ) {
    int fd = open("/dev/urandom", O_RDONLY);
    VRFY( fd > 0, );
    VRFY( count == read(fd, buf, count), );
    close(fd);
}

/* Note: These queues are non-blocking; If they are full, they will overrun
         data.  We try to mitigate the effects of an overrun by by copying
         the result to char[] msg; but that is obviously imperfect.

         The code does detect when the queue has been overrun, but it
         doesn't do anything with that information.

         While you can make the queue larger, it doesn't really help if
         the consumers are slower than the producers.

         The best approach if you are having overwritten messages is
         to have multiple log writers; but for that to work, you need
         to update the cursor to be a real tail using atomic operations.
  */

char* dtn_log_getnext() {
  static __thread int64_t cursor=0;
  static __thread char msg[ESCP_MSG_SZ];

  int64_t head = __sync_fetch_and_add( &ESCP_DTN_ARGS->debug_count, 0 );
  char* ptr;

  if ( cursor >= head )
    return NULL; // Ring Buffer Empty

  if ( (head > ESCP_MSG_COUNT) &&
      ((head - ESCP_MSG_COUNT) > cursor ) ) {

    // head has advanced past implicit tail
    cursor = head - ESCP_MSG_COUNT;
  }

  ptr = (char*) (((cursor % ESCP_MSG_COUNT)*ESCP_MSG_SZ)
                   + ESCP_DTN_ARGS->debug_buf);

  memcpy( msg, ptr, ESCP_MSG_SZ );
  cursor++;
  return msg;
}

char* dtn_err_getnext() {
  static __thread int64_t cursor=0;
  static __thread char msg[ESCP_MSG_SZ];

  int64_t head = __sync_fetch_and_add( &ESCP_DTN_ARGS->msg_count, 0 );
  char* ptr;

  if ( cursor >= head )
    return NULL; // Ring Buffer Empty

  if ( (head > ESCP_MSG_COUNT) &&
      ((head - ESCP_MSG_COUNT) > cursor ) ) {

    // head has advanced past implicit tail
    cursor = head - ESCP_MSG_COUNT;
  }

  ptr = (char*) (((cursor % ESCP_MSG_COUNT)*ESCP_MSG_SZ)
                   + ESCP_DTN_ARGS->msg_buf);

  memcpy( msg, ptr, ESCP_MSG_SZ );
  cursor++;
  return msg;
}



