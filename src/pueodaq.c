#define _GNU_SOURCE

#include "daqregs.h"
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <assert.h>
#include <math.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>

#include "pueodaq.h"
#include "pueodaq-net.h"
#include "pueodaq-private.h"

const fpga_id_t the_turfid = { .c = {'T','U','R','F'} };
const fpga_id_t the_surfid = { .c = {'S','U','R','F'} };
const fpga_id_t the_tfioid = { .c = {'T','F','I','O'} };


static int datever_dump(FILE * f, datever_t d)
{
  return fprintf(f,"v%02u.%02u.%02u %02u/%02u/2%03u",
      d.decoded.major, d.decoded.minor, d.decoded.rev,
      d.decoded.month, d.decoded.day, d.decoded.year);
}


static uint64_t pack_time(struct timespec ts)
{
  //truncate to 34 bits
  uint64_t secs = ts.tv_sec & (0x3ffffffff);
  //tv_nsec is always < 30 bits
  return secs << 34  | ts.tv_nsec;
}

void unpack_time(uint64_t val, struct timespec * ts)
{
  ts->tv_sec = val >> 34;
  ts->tv_nsec = val & (0x3fffffff);
}





struct blocking_wait_check
{
  uint64_t wanted;
  uint64_t wanted_mask;
  uint64_t val;
};

#define READ_WAIT_CHECK(addr,tag) ( (struct blocking_wait_check) {.wanted = (addr & 0x0fffffff) | (tag << 28), .wanted_mask = 0xffffffff } )
#define WRITE_WAIT_CHECK(addr,tag)( (struct blocking_wait_check) {.wanted = (addr & 0x0fffffff) | (tag << 28), .wanted_mask = 0xffffffff } )
#define ACK_WAIT_CHECK(addr,tag,allow) ( (struct blocking_wait_check) { .wanted = (  (((uint64_t) allow) << 63) | (1ull << 62) | ( ((uint64_t) addr) <<20) | (((uint64_t)tag) << 32)), .wanted_mask = 0xc00000fffff00000} )
#define CTL_WAIT_CHECK(ctl) ( ( struct blocking_wait_check)  {.wanted = ctl.RAW, .wanted_mask = 0xffffffffffff} )
#define PERMISSIVE_WAIT_CHECK() ( (struct blocking_wait_check)  { .wanted_mask = 0 } )


static void * control_thread(void * arg);
static void * reader_thread(void * arg);


typedef union acked_msg
{
  uint64_t u;
  turf_ctl_t c;
  turf_rdreq_t r;
  turf_rdresp_t p;
  turf_wrresp_t wp;
  turf_wrreq_t w;
  turf_ack_t a;
} acked_msg_t __attribute__((__transparent_union__));

typedef union acked_msg_ptr
{
  uint64_t *u;
  turf_ctl_t *c;
  turf_rdreq_t *r;
  turf_rdresp_t *p;
  turf_wrresp_t *wp;
  turf_wrreq_t *w;
  turf_ack_t *a;
  acked_msg_t * m;
} acked_msg_ptr_t __attribute__((__transparent_union__));



static int blocking_wait_for_response(pueo_daq_t * daq,  int sck, struct sockaddr_in *wr, int Nmsg, acked_msg_ptr_t rcv, struct blocking_wait_check * check)
{
  struct pollfd fd;
  fd.fd = sck;
  fd.events = POLLIN;
  int ready = ppoll (&fd, 1, &daq->cfg.timeout, NULL);
  if (!ready || !(fd.revents & POLLIN ))
  {
    if (daq->cfg.debug) fprintf(stderr,"bwait: Timeout reached..\n");
    return -1;
  }

  // no interest in reading, apparently
  if (!rcv.u && !check)
  {
    return 0;
  }

  //read it into the buf and maybe  do the check
  struct  sockaddr_in  src;
  socklen_t srclen = sizeof(src);

  ssize_t r = recvfrom(sck, rcv.u ?: &check->val ,  rcv.u ? Nmsg * sizeof(rcv) : sizeof(check->val) ,0, &src, &srclen);

  //make sure we got something
  if ( r <= 0)
  {
    if (daq->cfg.debug) fprintf(stderr,"bwait: r <=0 !!!");
    return -2;
  }

  //check to make sure the packet came from the turf
  if (src.sin_addr.s_addr != daq->net.turf_ip.s_addr) 
  {
    if (daq->cfg.debug) fprintf(stderr,"bwait: wrong ip");
    return -3;
  }

  // Check it's the correct port
  if (src.sin_port != wr->sin_port)
  {
    if (daq->cfg.debug) fprintf(stderr,"bwait: wrong port");
    return -4;
  }

  // copy to check if we got the whole thing
  if (rcv.u) check->val = rcv.u[0];

  //check for wanted mismatch
  if ( (check->wanted & check->wanted_mask) != (check->val & check->wanted_mask)) 
  {
    if (daq->cfg.debug) fprintf(stderr,"bwait: wanted fail. Got 0x%016lx, wanted 0x%016lx (mask 0x%016lx)\n", check->val, check->wanted, check->wanted_mask);
    return -5;
  }

  return 0;
}

#define LAUNDER_U64(x) *((uint64_t*) &x)



static_assert(sizeof(acked_msg_t) == sizeof(uint64_t), "aked_msg_t != 8 bytes");

static int acked_multisend(pueo_daq_t * daq, int sock, uint16_t port, size_t Nsend,
    acked_msg_ptr_t snd, acked_msg_ptr_t rcv, struct blocking_wait_check * check)
{

  if (daq->cfg.debug > 2)
  {
    printf("Sending %zu packets to turf:%hu\n > ", Nsend, port);
    for (unsigned i = 0; i < (Nsend > 4 ? 4 : Nsend) ; i++)
      printf("  0x%016lx", snd.u[i]);
    if (Nsend > 4) printf(" ...");
    printf("\n");
  }

  int ret = -1;
  struct sockaddr_in a = {.sin_addr = daq->net.turf_ip, .sin_port = htons(port)};
  for (unsigned attempt = 0; attempt < daq->cfg.max_attempts; attempt++)
  {
    pthread_mutex_lock(&daq->net.tx_lock);

    // read request is smaller than rest so have to be careful here
    size_t msg_size = port == TURF_PORT_READ_REQ ? sizeof(turf_rdreq_t) : sizeof(acked_msg_t);
    ssize_t sent = sendto(sock, snd.m, Nsend * msg_size,
                          0, (struct sockaddr*) &a, sizeof(a));
    pthread_mutex_unlock(&daq->net.tx_lock);
    if (sent != (ssize_t) (Nsend *msg_size))
    {
      fprintf(stderr,"Sending problem?\n");
      continue;
    }

    if (!blocking_wait_for_response(daq, sock, &a, Nsend, rcv, check))
    {
      if (daq->cfg.debug > 2 && rcv.u)
      {
        printf(" < ");
        for (unsigned i = 0 ; i < (Nsend > 4 ? 4 : Nsend) ; i++)
        {
          printf("  0x%016lx", rcv.u[i]);
        }
        if (Nsend > 4) printf(" ...");
        printf("\n");
      }
      ret = 0;
      break;
    }
  }

  return ret;
}

static int acked_send(pueo_daq_t * daq, int sock, uint16_t port,  acked_msg_t snd, acked_msg_ptr_t rcv, struct blocking_wait_check * check)
{
  return acked_multisend(daq, sock, port, 1, &snd, rcv, check);
}

static inline struct event_buf * event_buf_get(pueo_daq_t * daq, int i)
{
  return (struct event_buf*)  ( ((uint8_t*) daq->event_bufs) + i * daq->evbuf_sz);
}

static inline struct fragment * fragment_get(pueo_daq_t * daq, uint32_t i)
{
  return (struct fragment*)  (((uint8_t*) daq->fragments) + i * (sizeof(struct fragment) + daq->cfg.fragment_size));
}


void fragment_done( pueo_daq_t * daq, uint32_t i)
{
  assert (i < daq->nfragments);
  int w = i / 64;
  int b = i % 64;

  //clear bit
  atomic_fetch_and(&daq->fragments_bitmap[w], ~(1ull << b));
}


static void allow_ack(pueo_daq_t * daq, uint16_t addr)
{

  int w = addr / 64;
  int b = addr % 64;
  uint64_t m = 1ull << b;

  uint64_t was = atomic_fetch_or(&daq->ack_map[w], m);

  // check to make sure we're not double acking.
  assert (!(was  & m));

  //signal the ack thread if it's sleeping
//  pthread_kill(SIGUSR1, daq->ctl_thread);
}


void fragment_mark_free(pueo_daq_t * daq, const uint32_t *  fragments, unsigned nfragments)
{
   for (unsigned i = 0; i < nfragments; i++)
   {
      //TODO try to coalesce words, or something, though they'll be scattered all over the place
      uint32_t frag = fragments[i];
      uint32_t w = frag / 64;
      uint32_t b = frag % 64;
      atomic_fetch_and(&daq->fragments_bitmap[w], ~(1ull << b));
   }
}

uint16_t event_buf_find_free(pueo_daq_t * daq)
{

  int nwords = daq->evbuf_bitmap_size;;
  uint_fast16_t start_bit = atomic_load(&daq->last_free_evbuf);
  int start_word = start_bit / 64;
  int word = start_word;

  // we will block until we find one
  // we should never run out of events because we won't acknowledge an event until we have enough space for a new one.
  while(true)
  {
    volatile uint64_t was = atomic_load(&daq->event_bufs_inuse_bitmap[word]);
    while ((was & (1ull << 63)) == 0 ) //at least one bit at the end here
    {
      //this has to exist since we already asserted was is not full
      int next_free_bit = was == 0 ? 0 : 64 - __builtin_clzl(was);

      uint64_t update =  was | (1ull << next_free_bit);
      //we're in a loop anyway so we can use weak here
      if (atomic_compare_exchange_weak(&daq->event_bufs_inuse_bitmap[word],&was,update))
      {
        atomic_store(&daq->last_free_evbuf, word * 64 + next_free_bit);
        return word * 64 + next_free_bit;
      }
    }

    // next word
    word = (word + 1) % nwords;

    // should never happen.
    assert (word != start_word);
  }
}

uint16_t event_buf_find_ready(pueo_daq_t * daq)
{

  int nwords = daq->evbuf_bitmap_size;;
  uint_fast16_t start_bit = atomic_load(&daq->last_sent_evbuf);
  int start_word =  start_bit / 64;
  int word = start_word;

  // we will block until we find one
  // we should never run out of events because we won't acknowledge an event until we have enough space for a new one.
  while(true)
  {
    volatile uint64_t was = atomic_load(&daq->event_bufs_ready_bitmap[word]);
    while (was != 0ull)
    {
      //this has to exist since we already asserted was is not empty
      int first_marked_bit = __builtin_ctzl(was);

      uint64_t update =  was & ~(1ull << first_marked_bit);
      //we're in a loop anyway so we can use weak here
      if (atomic_compare_exchange_weak(&daq->event_bufs_ready_bitmap[word],&was,update))
      {
        atomic_store(&daq->last_sent_evbuf, word * 64 +first_marked_bit);
        return word * 64 + first_marked_bit;
      }
    }

    // next word
    word = (word + 1) % nwords;

    // should never happen.
    assert (word != start_word);
  }
}


void event_buf_idx_mark_free(pueo_daq_t * daq, uint32_t idx)
{
   uint32_t w = idx / 64;
   uint32_t b = idx % 64;
   atomic_fetch_and(&daq->event_bufs_inuse_bitmap[w], ~(1ull << b));
}

void event_buf_mark_free(pueo_daq_t * daq, struct event_buf * buf)
{
   uint32_t idx = (((char*) buf) - ((char*) daq->event_bufs)) / daq->evbuf_sz;
   event_buf_idx_mark_free(daq,idx);
}

void event_buf_mark_ready(pueo_daq_t * daq, struct event_buf * buf)
{
   uint32_t idx = (((char*) buf) - ((char*) daq->event_bufs)) / daq->evbuf_sz;
   uint32_t w = idx / 64;
   uint32_t b = idx % 64;
   atomic_fetch_or(&daq->event_bufs_ready_bitmap[w], (1ull << b));
}


static uint32_t fragment_find_free( pueo_daq_t * daq, uint8_t tnum)
{
  // We space each thread out in an attempt to reduce contention
  //
  int nwords = daq->fragments_bitmap_size;
  int start_word = (((nwords+daq->cfg.n_recvthreads-1)/daq->cfg.n_recvthreads) * tnum) % nwords;

  int word = start_word;

  // we will block until we find one
  // we should never run out of fragments because we won't acknowledge an event until we have enough space for a new one.
  while(true)
  {
    volatile uint64_t was = atomic_load(&daq->fragments_bitmap[word]);
    while (was != ~(0ull))
    {
      //this has to exist since we already asserted was is not full
      int first_free_bit = __builtin_ctzl(~was);

      uint64_t update =  was | (1ull << first_free_bit);
      //we're in a loop anyway so we can use weak here
      if (atomic_compare_exchange_weak(&daq->fragments_bitmap[word],&was,update))
      {
        return word * 64 + first_free_bit;
      }
    }

    // next word
    word = (word + 1) % nwords;

    // should never happen.
    assert (word != start_word);
  }
}

static uint32_t surprise = 0;
static struct  event_buf * event_buf_for_fragment(pueo_daq_t * daq, turf_fraghdr_t fhdr, bool *first)
{
  uint16_t addr = fhdr.BIT.ADDR;

  // check if -1
  volatile int_fast16_t was = atomic_load(&daq->addr_map[addr]);

  while (was == -1)
  {
    //let's set it to -2 which should signal to other threads that they should wait for us
    //if we fail to do so, maybe another thread has alreayd set it to -2 2

    if (atomic_compare_exchange_weak(&daq->addr_map[addr],&was, -2))
          break;
  }

  //another thread is initalizing this, let's wait for it, it shouldn't take long
  //should we use a mutex instead? I don't know. This should be like, really fast.
  while (was == -2)
  {
    usleep(1); ///
    was = atomic_load(&daq->addr_map[addr]);
  }

  if (was >=0 )
  {
    struct event_buf * ev= event_buf_get(daq, was);
    while(!atomic_load(&ev->first_fragment_packed_time)); // make sure the event is fully initialized
    return ev;
  }


  assert (was==-1);


  uint16_t free_event = event_buf_find_free( daq);

  int_fast16_t is = -2;



  // we'll be extra careful here. I don't THINK multiple threads can get here but...
  if (!atomic_compare_exchange_strong(&daq->addr_map[addr], &is, free_event))
  {
    surprise++;
    if (is >= 0) //ok someone else changed but that's ok, even though it doesn't seem like it should happen
    {
      //all that work for nothing!
      event_buf_idx_mark_free(daq, free_event);
      struct event_buf * ev = event_buf_get(daq,is % daq->cfg.n_event_bufs);
      while( (volatile uint64_t) !atomic_load(&ev->first_fragment_packed_time)); // make sure the event is fully initialized
      return ev;
   }

    // we done fucked up
    abort();

  }
  struct event_buf * ev = event_buf_get(daq, free_event);
  ev->nfragments_expected  = (fhdr.BIT.TOTAL + daq->cfg.fragment_size -1) / daq->cfg.fragment_size;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC,&now);
  uint64_t packed_now = pack_time(now);

  //TODO are there race conditions here?
  ev->run_number = daq->run_number;
  ev->event_index = atomic_fetch_add(&daq->num_events_discovered,1);
  ev->nbytes_expected = fhdr.BIT.TOTAL;
  //TODO: should we use SO_TIMESTAMP[NS]?
  ev->address = addr;
  atomic_store(&ev->first_fragment_packed_time, packed_now);
  *first = true;
  return ev;
}



static int event_buf_reset(struct event_buf * evbuf)
{
  // this should only happen in a single thread at a time so no need to use atomic stores
  evbuf->nfragments_expected = 0;
  evbuf->nfragments_rcvd = 0;
  evbuf->nbytes_expected = 0;
  evbuf->nsamples = 0;
  evbuf->header_size = 0;
  evbuf->event_index = 0;
  evbuf->run_number = 0;
  evbuf->first_fragment_packed_time = 0;
  evbuf->last_fragment_packed_time = 0;


  return 0;
}


static int pgsiz;
__attribute__((constructor))
static void set_pgsize()
{
  pgsiz = sysconf(_SC_PAGESIZE);
}



static const pueo_daq_config_t default_config = { PUEO_DAQ_CONFIG_DFLT } ;


pueo_daq_t * pueo_daq_init(const pueo_daq_config_t * cfg)
{
  if (!cfg)
  {
    cfg = &default_config;
  }

  struct in_addr our_ip = {0};
  if (pueo_daq_config_validate(cfg,stderr, &our_ip))
  {
    fprintf(stderr,"pueo_daq_init: Configuration did not validate!\n");
    return NULL;
  }

  pueo_daq_t * daq = calloc(1,sizeof(pueo_daq_t));
  if (!daq)
  {
    fprintf(stderr,"Could not allocate memory for pueo_daq_t!!!");
    return 0;
  }


  //copy the config in
  memcpy(&daq->cfg, cfg, sizeof(pueo_daq_config_t));
  daq->net.our_ip = our_ip;

  //set some defaults in config if necessary
  if (!daq->cfg.n_recvthreads) daq->cfg.n_recvthreads = default_config.n_recvthreads;


  //set up buffers
  // How many fragments do we need?
  uint32_t max_fragments_per_event = ceil(daq->cfg.max_ev_size * 1.0 / daq->cfg.fragment_size);
  daq->nfragments = max_fragments_per_event * daq->cfg.n_event_bufs;
  daq->fragments_bitmap_size = (daq->nfragments + 63)/64;


  errno = 0;
  daq->fragments = mmap(NULL,
      daq->nfragments * (sizeof(struct fragment) + daq->cfg.fragment_size),
      PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1,0);

  daq->fragments_bitmap = calloc(daq->fragments_bitmap_size, sizeof(*daq->fragments_bitmap));

  if (daq->fragments == (void*) -1 || !daq->fragments_bitmap)
  {
    fprintf(stderr,"Couldn't allocate fragments :( %s\n", strerror(errno));
    daq->fragments = NULL;
    goto bail;
  }

  //if our nfragments is not a multiple of 64, the last word will contain some unusable bits (sorry Lawrence)
  // so let's mark them as used
  if (daq->nfragments % 64)
  {
    // this minus 1 will be the complement to our mask
    uint64_t one_louder = 1ull << (daq->nfragments % 64);
    daq->fragments_bitmap[daq->fragments_bitmap_size-1] |= ~(one_louder-1);
  }


  daq->evbuf_sz = sizeof(struct event_buf) + max_fragments_per_event * sizeof(uint32_t);

  errno=0;
  daq->event_bufs = mmap( NULL, daq->cfg.n_event_bufs * daq->evbuf_sz,
                          PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE, -1, 0);

  if (daq->event_bufs ==(void*) -1)
  {
    fprintf(stderr,"Couldn't mmap event_bufs :( %s\n", strerror(errno));
    daq->event_bufs = NULL;
    goto bail;
  }

  daq->evbuf_bitmap_size = (daq->cfg.n_event_bufs + 63) /64;
  daq->event_bufs_inuse_bitmap = calloc(daq->evbuf_bitmap_size, sizeof(*daq->event_bufs_inuse_bitmap));
  daq->event_bufs_ready_bitmap = calloc(daq->evbuf_bitmap_size, sizeof(*daq->event_bufs_ready_bitmap));

  // set turf address/mask

  if (!inet_aton(cfg->turf_ip_addr, &daq->net.turf_ip))
  {
    fprintf(stderr, "Could not interpret %s as an IPV4 address\n", cfg->turf_ip_addr);
    goto bail;
  }

  if (cfg->turf_subnet_len > 32)
  {
    fprintf(stderr, "Could not interpret /%u as an IPV4 subnet legnth\n", cfg->turf_subnet_len);
    goto bail;
  }


  // open UDP sockets
#define SETUP_SOCK(which,port,opts, opt_vals, dest_port) \
  do {\
    daq->net.which = socket(AF_INET, SOCK_DGRAM, 0);\
    if (daq->net.which < 0) { \
      fprintf(stderr, "Couldn't open socket %s\n", #which); \
      goto bail; \
    }\
    if (opts != NULL) { \
      int iopt = 0; int defaultoptval = 1;\
      while( ((int*) opts)[iopt])\
      {\
        if (setsockopt(daq->net.which, SOL_SOCKET, ((int*)opts)[iopt], opt_vals != NULL ? &((int*)opt_vals)[iopt] : &defaultoptval, sizeof(int)))\
        {\
          fprintf(stderr,"Couldn't setsockopt :(\n"); goto bail;\
        }\
        iopt++;\
      }\
    }\
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {.s_addr = htonl(INADDR_ANY)} }; \
    if (bind(daq->net.which,(struct sockaddr*) &addr, sizeof(addr))) { \
     fprintf (stderr,"Couldn't bind %s to %d\n", #which, port);\
     goto bail; \
    }\
    if (dest_port) {\
      addr.sin_port = htons(dest_port);\
      addr.sin_addr.s_addr = daq->net.turf_ip.s_addr;\
      if (connect (daq->net.which, (struct sockaddr*) &addr, sizeof(addr))) { \
        fprintf(stderr, "Coudln't connect to %s:%hhu\n", daq->cfg.turf_ip_addr, dest_port);\
        goto bail;\
      }\
    }\
  } while(0)


  SETUP_SOCK(daq_ctl_sck, daq->cfg.daq_ports.control_in,0,0, 0);
  SETUP_SOCK(daq_frgctl_sck, daq->cfg.daq_ports.fragcontrol_in,0,0, 0);

  for (unsigned i = 0; i < daq->cfg.n_recvthreads; i++ )
  {
    //TODO: we should protect against multiple instances here by using some sort of external lock on the port
    int opts[] = { SO_RCVBUF, daq->cfg.n_recvthreads == 1 ? 0 : SO_REUSEPORT, 0 };
    int opt_vals[] = {  (4 << 20) , 1 , 0 };
    SETUP_SOCK(daq_frg_sck[i], daq->cfg.daq_ports.fragment_in,opts,opt_vals,0);
  }

  //set up dest addresses

  //set up locks
  pthread_mutex_init(&daq->net.tx_lock,0);
  daq->run_number = 0;

  sigset_t newset, oldset;
  sigemptyset(&newset);
  sigaddset(&newset, SIGINT);
  sigaddset(&newset, SIGTERM);

  //set up threads
  pthread_sigmask(SIG_BLOCK, &newset, &oldset) ;

  if (pthread_create(&daq->ctl_thread, NULL, control_thread, daq))
  {
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
    fprintf(stderr,"Could not create ctl thread\n");
    goto bail;
  }

  for (int i = 0; i < daq->cfg.n_recvthreads; i++)
  {
    daq->reader_thread_setups[i].tnum = i,
    daq->reader_thread_setups[i].daq = daq;
    if (pthread_create(&daq->reader_threads[i], NULL, reader_thread, &daq->reader_thread_setups[i]))
    {
      fprintf(stderr, "Could not create reader_thread_%d\n", i);
      pthread_sigmask(SIG_SETMASK, &oldset, NULL);
      goto bail;
    }
  }

  pthread_sigmask(SIG_SETMASK, &oldset, NULL);

 
  // this prepares the event interface

  if (pueo_daq_reset(daq))
  {
    fprintf(stderr,"Problem with DAQ reset\n");
    goto bail;
  }

  return daq;

  // on failure, we clean up after ourselves like good citizens
bail:
  pueo_daq_destroy(daq);
  return 0;


}

int pueo_daq_start(pueo_daq_t * daq)
{
  atomic_store(&daq->state, PUEODAQ_STARTING);


  // read and write the bit
  uint32_t reset = 0;
  if (read_turf_reg(daq, &turf_event.event_in_reset, &reset))
  {
    fprintf(stderr,"Could_not read event_in_reset\n");
    return 1;
  }
  if (!reset)
  {
    if (write_turf_reg(daq, &turf_event.event_reset, 1)
        || write_turf_reg(daq, &turf_event.event_reset, 0))
    {
      fprintf(stderr,"Could not write event_reset\n");

      return 1;
    }

  }
  else {
    printf("Already in reset???\n");
  }

  // open the event interface
  turf_ctl_t ctl  = {.BIT =
    {
      .COMMAND = TURF_OP_COMMAND,
      .PAYLOAD = ((uint64_t)(daq->cfg.daq_ports.fragment_in)) | ( ((uint64_t)htonl(daq->net.our_ip.s_addr)) << 16)
    }
  };

  if (acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl, NULL, &CTL_WAIT_CHECK(ctl)))
  {
    fprintf(stderr,"Problem with open\n");
    return 1;
  }


 //turfio mask, etc.
  if (write_turf_reg(daq, &turf_event.mask, daq->cfg.trigger.turfio_mask))
  {
    fprintf(stderr,"Couldn't write turfio mask\n");
  }

  if (daq->cfg.trigger.turfio_mask != 0xf)
  {
    if (daq->cfg.trigger.apply_latency)
    {
      if (write_turf_reg(daq, &turf_trig.latency, daq->cfg.trigger.latency))
      {
        fprintf(stderr,"Couldn't write trig latency\n");
      }
    }

    if (daq->cfg.trigger.apply_offset)
    {
      if (write_turf_reg(daq, &turf_trig.offset, daq->cfg.trigger.offset))
      {
        fprintf(stderr,"Couldn't write trig offset\n");
      }
    }
    if (daq->cfg.trigger.apply_holdoff)
    {
      if (write_turf_reg(daq, &turf_trig.holdoff, daq->cfg.trigger.holdoff))
      {
        fprintf(stderr,"Couldn't write trig holdoff\n");
      }
    }
 
  }


  if (daq->cfg.trigger.apply_rundly)
  {
    //set up rundly
    if (write_turf_reg(daq, &turf_trig.rundly, daq->cfg.trigger.rundly))
    {
      fprintf(stderr,"Coudn't set rundly\n");
    }
  }

  //setup all acks, batch in groups of TURF_MAX_ACKS
  int nallow = daq->cfg.max_in_flight;
  daq->num_events_allowed = nallow;
  for (unsigned i = 0; i < TURF_NUM_ADDR; i+=TURF_MAX_ACKS)
  {
    turf_ack_t acks[TURF_MAX_ACKS] = {};
    for (unsigned j = 0 ; j < TURF_MAX_ACKS; j++)
    {
      acks[j].BIT.ADDR = i+j;
      acks[j].BIT.TAG = i + j;
      acks[j].BIT.ALLOW = !!(nallow-- > 0);
      daq->addr_map[i+j] = -1;
    }

    if (acked_multisend(daq, daq->net.daq_frgctl_sck, TURF_PORT_ACK, TURF_MAX_ACKS, acks,
          NULL,  &ACK_WAIT_CHECK(acks[TURF_MAX_ACKS-1].BIT.ADDR, acks[TURF_MAX_ACKS-1].BIT.TAG, acks[TURF_MAX_ACKS-1].BIT.ALLOW)))
    {
      fprintf(stderr,"Problem sending acks\n");
      return 1;
    }
  }

  daq->num_addr_assigned = 0;
  if (write_turf_reg(daq, &turf_trig.runcmd, RUNCMD_RESET))
  {
    fprintf(stderr,"Could not run runcmd\n");
    return 1;

  }
  atomic_store(&daq->state, PUEODAQ_RUNNING);

  return 0;
}

int pueo_daq_stop(pueo_daq_t * daq)
{
  volatile int state = atomic_load(&daq->state);
  if (state == PUEODAQ_RUNNING || state == PUEODAQ_UNINIT)
  {
    if (write_turf_reg(daq, &turf_trig.runcmd, RUNCMD_STOP))
    {
      fprintf(stderr,"Could not run runcmd\n");
      return 1;
    }
    //try to interrupt the recvs
    for (int i = 0; i < daq->cfg.n_recvthreads; i++)
    {
      pthread_kill(daq->reader_threads[i], SIGUSR1);
    }

  }


  // close the event interface
  turf_ctl_t ctl  = {.BIT = { .COMMAND = TURF_CL_COMMAND }};
  int ret = acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl, NULL, &CTL_WAIT_CHECK(ctl));
  atomic_store(&daq->state, PUEODAQ_IDLE);

  return ret;
}

int pueo_daq_reset(pueo_daq_t * daq)
{

  //make sure we're stopped
  pueo_daq_stop(daq);


  //If we have seen events before, we have to clear the event bufs
  if (atomic_load(&daq->num_events_discovered))
  {
    for (int i = 0; i < daq->cfg.n_event_bufs; i++)
    {
      event_buf_reset(event_buf_get(daq,i));
    }
  }

  daq->num_events_discovered = 0;
  daq->num_events_received = 0;
  daq->num_events_dispensed = 0;
  daq->num_events_dispense_began = 0;
  daq->num_acks_sent = 0;
  daq->num_events_allowed = 0;

  // reset tags on TURFS
  read_turf_reg(daq,&turf.turfid,&daq->census.turfid);

  if (FPGAID(daq->census.turfid).u != the_turfid.u)
  {
    fprintf(stderr,"TURF does not present as a TURF...this will probably end poorly\n");
  }

  //read date version
  read_turf_reg(daq,&turf.dateversion,&daq->census.turf_datever.as_uint);

  daq->census.turf_dna = read_dna(daq, TURF_BASE, &turf.dna);

  if (daq->cfg.debug > 1)
  {
    printf("TURFID: %4sx\n", FPGAID(daq->census.turfid).c);
    printf("TURF DATEVER: ");
    datever_dump(stdout, daq->census.turf_datever);
    printf("\n");
    printf("TURF DNA: %lu\n", daq->census.turf_dna);
  }

  //Take a census of who we have

  for (int itfio = 0; itfio < NTFIO; itfio++)
  {
    read_turfio_reg(daq, itfio, &turfio.turfioid, &daq->census.turfio[itfio].turfioid);
    if (FPGAID(daq->census.turfio[itfio].turfioid).u !=  the_tfioid.u)  
    {
      if (daq->cfg.debug)
      {
        printf("TFIO%d not correct id, assuming mising\n", itfio); 
      }
      daq->census.turfio[itfio].turfioid = 0;
    }

    else
    {
      read_turfio_reg(daq, itfio, &turfio.dateversion, &daq->census.turfio[itfio].turfio_datever.as_uint);
      daq->census.turfio[itfio].turfio_dna = read_dna(daq, TURFIO_BASE(itfio), &turfio.dna);

      if (daq->cfg.debug > 1)
      {

        printf("TURFIO%d ID: %4s\n", itfio, FPGAID(daq->census.turfio[itfio].turfioid).c);
        printf("TURFIO%d  DATEVER: ", itfio);
        datever_dump(stdout, daq->census.turfio[itfio].turfio_datever);
        printf("\n");
        printf("TURFIO%d DNA: %lu\n", itfio, daq->census.turfio[itfio].turfio_dna);
      }

      for (int isurf = 0; isurf < NSURFSLOTS; isurf++)
      {

        read_surf_reg(daq,SURF(itfio, isurf), &surf.surfid, &daq->census.turfio[itfio].surfid[isurf]);
        if (FPGAID(daq->census.turfio[itfio].surfid[isurf]).u  !=  the_surfid.u)  
        {
          if (daq->cfg.debug)
          {
            printf("TFIO%d-SURF%d not correct id, assuming mising\n", itfio, isurf); 
          }
          daq->census.turfio[itfio].surfid[isurf] = 0;
        }
        else
        {
          read_surf_reg(daq,SURF(itfio, isurf), &surf.dateversion, &daq->census.turfio[itfio].surf_datever[isurf].as_uint);
          daq->census.turfio[itfio].surf_dna[isurf] = read_dna(daq, SURF_BASE(itfio, isurf), &surf.dna);

          if (daq->cfg.debug > 1)
          {
            printf("TURFIO%d-SURF%d ID: %4s\n", itfio, isurf, FPGAID(daq->census.turfio[itfio].surfid[isurf]).c);
            printf("TURFIO%d-SURF%d  DATEVER: ", itfio, isurf);
            datever_dump(stdout, daq->census.turfio[itfio].surf_datever[isurf]);
            printf("\n");
            printf("TURFIO%d-SURF%d  DNA: %lu\n", itfio, isurf, daq->census.turfio[itfio].surf_dna[isurf]);
          }
        }
      }
    }
  }

  // close the event interface
  pueo_daq_stop(daq);

  // get max fragments

  turf_ctl_t ctl_rd = { .BIT.COMMAND = TURF_PR_COMMAND };
  turf_ctl_t ctl_wr = { .BIT.COMMAND = TURF_PW_COMMAND };

  if (acked_send(daq,daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl_rd, &ctl_rd, &PERMISSIVE_WAIT_CHECK()))
  {
      fprintf(stderr,"Problem calling PR\n");
      return 1;
  }
  if (daq->cfg.debug > 1) 
    printf("PR: 0x%lx [fragsrcmask: 0x%lx addr: %lu  fraglen: %lu]\n",  ctl_rd.RAW,  ctl_rd.RAW & 0xffff, 1+((ctl_rd.RAW >> 16) & 0xffff), (8 +(( ctl_rd.RAW >> 32) &0xffff)) & 0xff80);

  uint64_t wr_payload = (daq->cfg.fragment_size-1) & 0xfff8; //drop lowest 3 bits?
  wr_payload <<=32;
  wr_payload |= daq->cfg.frag_src_mask;
  ctl_wr.BIT.PAYLOAD =wr_payload;
  turf_ctl_t ctl_check = {.RAW = wr_payload | 0xfff0000};
  if (acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl_wr, &ctl_wr, &CTL_WAIT_CHECK(ctl_check)))
  {
      fprintf(stderr,"Problem calling PW\n");
      return 1;
  }

  if (daq->cfg.debug > 1) printf("PW: 0x%lx [fragsrcmask: 0x%lx  addr: %lu  fraglen: %lu]\n", ctl_wr.RAW,  ctl_wr.RAW & 0xffff, 1+((ctl_wr.RAW >> 16) & 0xffff), ( 8+ (( ctl_wr.RAW >> 32) &0xffff)) & 0xff80);


  return 0;
}



void pueo_daq_destroy(pueo_daq_t * daq)
{
  if (!daq) return;


  //wait for receive threads
  atomic_store(&daq->state,PUEODAQ_STOPPING);

  for (unsigned i = 0; i < daq->cfg.n_recvthreads; i++)
  {
    // send a signal to each thread to get it to stop
    if (daq->reader_thread_setups[i].daq)
      pthread_cancel(daq->reader_threads[i]);
  }
  for (unsigned i = 0; i < daq->cfg.n_recvthreads; i++)
  {
    if (daq->reader_thread_setups[i].daq)
      pthread_join(daq->reader_threads[i], NULL);
    close(daq->net.daq_frg_sck[i]);
  }

  // Send a CL
  // close the event interface
  turf_ctl_t ctl  = {.BIT = { .COMMAND = TURF_CL_COMMAND }};
  if ( acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl, NULL, &CTL_WAIT_CHECK(ctl)))
  {
    fprintf(stderr,"Trouble closing\n");
  }



  //close non-thread sockets
  close(daq->net.daq_ctl_sck);
  close(daq->net.daq_frgctl_sck);

  //deallocate buffers, if they exist
  if (daq->event_bufs)
  {
    munmap(daq->event_bufs, daq->evbuf_sz * daq->cfg.n_event_bufs);

  }

  if (daq->fragments)
  {
    munmap(daq->fragments, daq->nfragments * (sizeof(struct fragment) + daq->cfg.fragment_size));
  }

  if (daq->fragments_bitmap) free(daq->fragments_bitmap);


  //deallocate the daq
  free(daq);
}




int pueo_daq_write(pueo_daq_t * daq, uint32_t wraddr, uint32_t data)
{
  uint32_t tag =  atomic_fetch_add(&daq->net.wr_tag,1)  & 0xf;
  turf_wrreq_t msg= {.BIT={.ADDR = wraddr & 0x0fffffff, .TAG = tag, .WRDATA = data}};
  int r = acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_WRITE_REQ, msg, NULL, &WRITE_WAIT_CHECK(wraddr, tag));

  return r;
}


int pueo_daq_write_many(pueo_daq_t *daq, const pueo_daq_many_setup_t * s)
{
  if (!s || s->N > PUEODAQ_MAX_MANY_SIZE) return -1;
  if (!s->N) return 0;

  static __thread turf_wrreq_t msgs[PUEODAQ_MAX_MANY_SIZE];
  static __thread turf_wrresp_t resps[PUEODAQ_MAX_MANY_SIZE];
  uint32_t tag =  atomic_fetch_add(&daq->net.wr_tag,1)  & 0xf;
  int addr_stride = s->addr_stride ?: 1;
  int data_stride = s->data_stride ?: 1;

  for (unsigned i = 0; i < s->N; i++)
  {
    msgs[i].BIT.ADDR =  s->addr_v ? s->addr_v[addr_stride*i] : s->addr_start + s->addr_step *i;
    msgs[i].BIT.ADDR += s->addr_offset + ( s->addr_offset_v ? s->addr_offset_v[i*addr_stride] : 0);
    msgs[i].BIT.WRDATA = s->wr_data_v ? s->wr_data_v[i*data_stride] :
                         s->wr_indirect_data_v ? *s->wr_indirect_data_v[i * data_stride] :
                         s->wr_val;
    msgs[i].BIT.TAG = tag;
  }

   return acked_multisend(daq, daq->net.daq_ctl_sck, TURF_PORT_READ_REQ, s->N, msgs, resps, &WRITE_WAIT_CHECK( msgs[0].BIT.ADDR, tag));
}

int pueo_daq_read_many(pueo_daq_t *daq, const pueo_daq_many_setup_t * s)
{
  if (!s || s->N > PUEODAQ_MAX_MANY_SIZE) return -1;
  if (!s->N) return 0;

  static __thread turf_rdreq_t msgs[PUEODAQ_MAX_MANY_SIZE];
  static __thread turf_rdresp_t resps[PUEODAQ_MAX_MANY_SIZE];
  uint32_t tag =  atomic_fetch_add(&daq->net.rd_tag,1)  & 0xf;
  int addr_stride = s->addr_stride ?: 1;
  int data_stride = s->data_stride ?: 1;

  for (unsigned i = 0; i < s->N; i++)
  {
    msgs[i].BIT.ADDR =  s->addr_v ? s->addr_v[addr_stride*i] : s->addr_start + s->addr_step *i;
    msgs[i].BIT.ADDR += s->addr_offset + ( s->addr_offset_v ? s->addr_offset_v[i*addr_stride] : 0);
    msgs[i].BIT.TAG = tag;
  }

  int r = acked_multisend(daq, daq->net.daq_ctl_sck, TURF_PORT_READ_REQ, s->N, msgs, resps, &READ_WAIT_CHECK( msgs[0].BIT.ADDR, tag));

  if (s->rd_data_v || s->rd_indirect_data_v)
  {
    for (unsigned i = 0; i < s->N; i++)
    {
       if (s->rd_data_v) s->rd_data_v[data_stride *i] = resps[i].BIT.RDDATA;
       else  *s->rd_indirect_data_v[data_stride*i] = resps[i].BIT.RDDATA;
    }
  }
  return r;
}


int pueo_daq_read(pueo_daq_t * daq, uint32_t rdaddr, uint32_t *data)
{
  uint32_t tag =  atomic_fetch_add(&daq->net.rd_tag,1)  & 0xf;
  turf_rdreq_t msg= {.BIT={.ADDR = rdaddr & 0x0fffffff, .TAG = tag}};
  turf_rdresp_t resp;
  int r = acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_READ_REQ, msg, &resp, &READ_WAIT_CHECK(rdaddr, tag));
  if (data) *data = resp.BIT.RDDATA;

  return r;
}
void signore(int sig)
{
  (void) sig;
}

void * reader_thread(void *arg)
{

  struct reader_thread_setup *s = (struct reader_thread_setup*) arg;
  pueo_daq_t * daq = s->daq;
  int sck_frg = daq->net.daq_frg_sck[s->tnum];

  signal(SIGUSR1, signore);

  while(1)
  {
    volatile int state = atomic_load(&daq->state);
    // we should do nothing
    if (state == PUEODAQ_IDLE || state == PUEODAQ_UNINIT || state == PUEODAQ_STARTING)
    {
        usleep(10000);
        continue;
    }

    // we should stop doing anything
    if (state == PUEODAQ_ERROR || state == PUEODAQ_STOPPING)
    {
      break;
    }

    //otherwise we're going to try to receive stuff
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);;
    //get the index of a free fragment
    uint16_t frag_i = fragment_find_free(daq, s->tnum);
    struct fragment * frag = fragment_get(daq, frag_i);
    if (daq->cfg.debug > 2 )printf("Thd  %d trying to receive fragment %d (0x%p)\n", s->tnum, frag_i, frag);

    errno = 0;
    ssize_t nrecv = recvfrom(sck_frg, frag, daq->cfg.fragment_size + sizeof(struct fragment),0, (struct sockaddr*) &src, &src_len);

    if (nrecv  < 0)
    {
      printf("thd %d: %s\n", s->tnum,strerror(errno));
      continue; //TODO do better, though often this is EINTR
    }

    uint16_t fragnum = frag->hd.BIT.FRAGMENT;
    uint16_t addr = frag->hd.BIT.ADDR;
    //uint32_t kid = frag->hd.BIT.KID;
//    uint32_t evlen = frag->hd.BIT.TOTAL;

    //this will do some initializaiton if the first event
    bool first = false;
    struct event_buf * ev = event_buf_for_fragment(daq,  frag->hd, &first);
    if (daq->cfg.debug > 0 && first) printf("thd %d recvd %ld bytes from unencountered address (frag %d, addr %hu, evbuf: 0x%p)\n", s->tnum, nrecv, frag_i, addr, ev);

    struct timespec now;
    // this will be used by the nack detector
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t packed_now = pack_time(now);
    atomic_store(&ev->last_fragment_packed_time, packed_now);
    ev->fragments[fragnum] = frag_i;

    if (fragnum == 0) //do some preprocessing on header
    {
      int surf_header_size = frag->buf[frag->buf[0]];
      ev->header_size =  (surf_header_size + frag->buf[0] +1)*2;
      ev->nsamples = (ev->nbytes_expected -  ev->header_size) / PUEODAQ_NCHAN / 2;
    }


    //add fragments received
    uint16_t prior = atomic_fetch_add(&ev->nfragments_rcvd, 1);
    if (daq->cfg.debug > 1) printf("[fragnum=%hu][addr=%hu](nrecv=%hu/%hu) @ %ld.%09ld\n",fragnum, addr, 1+prior, ev->nfragments_expected, now.tv_sec, now.tv_nsec);

    // we finished.I think this is ok since we can't get more than nfagments_expected fragments, right?
    if (prior == ev->nfragments_expected -1)
    {
      uint32_t rcv_idx = atomic_fetch_add(&daq->num_events_received, 1);

      if (daq->cfg.debug > 0) printf("thd %d recvd last %ld bytes from address (frag %d, addr %hu, evbuf: 0x%p, expected: %hu)\n", s->tnum, nrecv, frag_i, addr, ev, ev->nfragments_expected );

      // schedule an ack if we have room for another event
      event_buf_mark_ready(daq,ev);
      allow_ack(daq, ev->address);
      if (daq->cb) daq->cb(daq, rcv_idx);
    }



  }

  return 0;
}

int pueo_daq_dump(pueo_daq_t * daq, FILE * stream, int flags)
{
  (void) flags;

  int r = 0;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
#define ATOMIC_PRINT(x) r+= fprintf(stream,#x ": %u\n", atomic_load(&daq->x))
  fprintf(stream, "DAQ DUMP @ %ld.%09ld:\n", ts.tv_sec, ts.tv_nsec);
  ATOMIC_PRINT(num_events_discovered);
  ATOMIC_PRINT(num_events_received);
  ATOMIC_PRINT(num_events_dispensed);

  pueo_daq_stats_t st;
  pueo_daq_get_stats(daq, &st);

  fprintf(stream, "turf_stats{\n" PUEODAQ_STATS_JSON_FORMAT"}\n", PUEODAQ_STATS_VALS(st));

  uint32_t pps_reg = 0;
  read_turf_reg(daq, &turf_trig.pps_reg, &pps_reg);
  fprintf(stream, " pps_reg = 0x%x\n", pps_reg);

  pueo_daq_scalers_t sc;
  pueo_daq_get_scalers(daq, &sc);
  pueo_daq_scalers_dump(stream, &sc);

  pueo_L2_stat_t l2;
  if (!pueo_daq_read_L2_stat(daq, &l2))
  {
    pueo_daq_L2_stat_dump(stream,(&l2));
  }


  if (flags & PUEODAQ_DUMP_INCLUDE_L1)
  {
    for (int turfio = 0; turfio < NTFIO ; turfio++)
    {
      if (!daq->census.turfio[turfio].turfioid) continue;
      for (int slot = 0; slot < NSURFSLOTS ; slot++)
      {
        if (!daq->census.turfio[turfio].surfid[slot]) continue;
        pueo_L1_stat_t l1 = {0};
        if (!pueo_daq_read_L1_stat(daq, turfio, slot, &l1))
          pueo_daq_L1_stat_dump(stream, &l1);
      }
    }
  }

  return r;
}

int pueo_daq_nready(const pueo_daq_t * daq)
{
  volatile uint32_t rcv = atomic_load(&daq->num_events_received);
  volatile uint32_t dis = atomic_load(&daq->num_events_dispensed);
  return rcv - dis;
}

void pueo_daq_wait_event(pueo_daq_t * daq)
{
  // TODO use a condition variable here? 
  // but that won't mesh well with our atomics design though...
  while (!pueo_daq_nready(daq)) usleep(500);
}

int pueo_daq_get_event(pueo_daq_t * daq, pueo_daq_event_data_t * dest)
{
  pueo_daq_wait_event(daq);

  atomic_fetch_add(&daq->num_events_dispense_began,1);
  uint16_t idx = event_buf_find_ready(daq);
  struct event_buf * ev = event_buf_get(daq, idx);

  assert(ev->header_size);

  //only support this for now, could handle truncated easily, adjustable with a bit more difficulty.
  assert (ev->nsamples == PUEODAQ_NSAMP);
  if (daq->cfg.debug) printf("Header size: %hu, nbytes_expected: %u\n", ev->header_size, ev->nbytes_expected);

  if (dest)
  {
          //header will always fit in first fragment
          memcpy(&dest->header, fragment_get(daq, ev->fragments[0])->buf, ev->header_size);

          uint32_t last_fragment_size = ev->nbytes_expected % (daq->cfg.fragment_size);


          void * p = &dest->waveform_data[0][0];
          p = mempcpy(p, fragment_get(daq,ev->fragments[0])->buf + ev->header_size/2, daq->cfg.fragment_size-ev->header_size);
          for (int i = 1; i < ev->nfragments_expected; i++)
          {
              p = mempcpy( p, fragment_get(daq,ev->fragments[i])->buf, i == ev->nfragments_expected - 1 ? last_fragment_size : daq->cfg.fragment_size);
          }
  }

  // mark fragments as free
  fragment_mark_free(daq, ev->fragments, ev->nfragments_rcvd);

  //reset the event buf
  event_buf_reset(ev);

  //all done, we can release the event buf now
  event_buf_mark_free(daq,ev);

  atomic_fetch_add(&daq->num_events_dispensed,1);

  return 0;

}

void pueo_daq_register_ready_callback(pueo_daq_t * daq, pueo_daq_event_ready_callback_t cb)
{
  daq->cb = cb;
}


void pueo_daq_set_run_number(pueo_daq_t * daq, uint32_t run)
{
  daq->run_number = run;
}


void* control_thread(void * arg)
{
  pueo_daq_t * daq = (pueo_daq_t*) arg;

  // we use this to interrupt the sleep
  signal(SIGUSR1, signore);

  // allow up to TURF_MAX_ACKS acqs at a time since why not

  turf_ack_t acks[TURF_MAX_ACKS] = {};
  unsigned num_acks = 0;

  //more than one nack is overkill
  turf_nack_t nack;
  uint8_t tag = 0;

  size_t loop_counter = 0;

  while(true)
  {

    volatile int state = atomic_load(&daq->state);
    if (state == PUEODAQ_STOPPING || state == PUEODAQ_ERROR)
    {
      break;
    }

    if (state == PUEODAQ_IDLE || state == PUEODAQ_UNINIT || state == PUEODAQ_STARTING)
    {
      usleep(10000); // sleep 10 ms
      continue;
    }

    // Do we have room to ack?
    // Note that we are the only thread (other than the main thread) that touches num_events_allowed
    volatile uint32_t dispensed = atomic_load(&daq->num_events_dispensed);
    volatile uint32_t discovered = atomic_load(&daq->num_events_discovered);

    assert(dispensed <= daq->num_events_allowed);

    // we have room for more events if the number allowed minus the number
    // dispensed is less than to the number of event bufs we  have available
    uint32_t event_bufs_used = discovered - dispensed;
    int event_bufs_available = daq->cfg.n_event_bufs - event_bufs_used - daq->cfg.max_in_flight;

    assert (event_bufs_available >=0);
    uint32_t capacity = event_bufs_available < 0 ? 0: event_bufs_available;

    if (capacity && (dispensed > daq->num_acks_sent))
    {
      num_acks = 0;
      for (unsigned w = 0; w < sizeof(daq->ack_map) / sizeof(*daq->ack_map); w++)
      {
        if (num_acks >=TURF_MAX_ACKS || num_acks >= capacity) break;
        volatile uint64_t word = atomic_load(&daq->ack_map[w]);

        if (word != 0) // we have a candidate to ack!
        {
          for ( int bit = 0; bit < 64; bit++)
          {
            if (num_acks >= TURF_MAX_ACKS || num_acks >= capacity) break;
            if (word & (1ull << bit))
            {
              uint16_t addr = w*64 +bit;
              acks[num_acks].BIT.ADDR = addr;
              acks[num_acks].BIT.TAG = tag;
              acks[num_acks].BIT.ALLOW = 1;
              num_acks++;
            }
          }
        }
      }

      if (num_acks > 0)
      {
        if (acked_multisend(daq, daq->net.daq_frgctl_sck, TURF_PORT_ACK, num_acks, acks, NULL, &ACK_WAIT_CHECK(acks[TURF_MAX_ACKS-1].BIT.ADDR, acks[TURF_MAX_ACKS-1].BIT.TAG, acks[TURF_MAX_ACKS-1].BIT.ALLOW)))
        {
            fprintf(stderr,"Problem acking\n");
            continue;
        }

        //ack successful!
        daq->num_acks_sent+=num_acks;
        daq->num_events_allowed += num_acks;
        //these count basically the same thing
        tag++;

        //increment the addresses, clear the ack bis
        for (unsigned i = 0; i < num_acks; i++)
        {
          uint16_t addr = acks[i].BIT.ADDR;
          if (daq->cfg.debug > 1) printf("Sent ack for %hu\n", addr);
          atomic_store(&daq->addr_map[addr],-1); //maybe doesn't need to be atomic?
          atomic_fetch_and(&daq->ack_map[addr/64], ~(1ull << (addr % 64)));
        }
      }

    }
    else if (loop_counter > 0 &&  (loop_counter % 1000 == 0))  // run the loop counter every once in a while. Maybe we'll eventualy make it configurable. 
    {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      // loop over used event fragments, see if any of them are started but not finished

    }
    else
    {
      usleep(100);
    }


    loop_counter++;

    // TODO go through and see if there is anything really old that needs to be nacked
  }
  return NULL;
}


int pueo_daq_soft_trig(pueo_daq_t * daq)
{
  return write_turf_reg(daq, &turf_trig.softrig, 1);
}


int pueo_daq_enable_rf_readout(pueo_daq_t * daq, bool enable, bool or_logic)
{
   return write_turf_reg(daq, &turf_trig.rf_trig_en, enable) ||
          write_turf_reg(daq, &turf_trig.leveltwo_logic, or_logic);
}


int pueo_daq_get_stats(pueo_daq_t * daq, pueo_daq_stats_t * st)
{
  uint32_t in_reset;
  uint32_t running;
  event_count_reg_t counts;
  uint32_t pps_reg = 0;
  uint32_t offset;
  uint32_t latency;
  uint32_t photoshutter_reg;
  holdoff_reg_t holdoff;
  if (
      read_turf_reg(daq, &turf_event.ndwords0, &st->turfio_words_recv[0]) ||
      read_turf_reg(daq, &turf_event.ndwords1, &st->turfio_words_recv[1]) ||
      read_turf_reg(daq, &turf_event.ndwords2, &st->turfio_words_recv[2]) ||
      read_turf_reg(daq, &turf_event.ndwords3, &st->turfio_words_recv[3]) ||
      read_turf_reg(daq, &turf_event.outqwords, &st->qwords_sent) ||
      read_turf_reg(daq, &turf_event.outevents, &st->events_sent) ||
      read_turf_reg(daq, &turf_trig.trigger_count, &st->trigger_count)||
      read_turf_reg(daq, &turf_trig.occupancy, &st->occupancy)||
      read_turf_reg(daq, &turf_time.current_second, &st->current_second)||
      read_turf_reg(daq, &turf_time.last_pps, &st->last_pps)||
      read_turf_reg(daq, &turf_time.llast_pps, &st->llast_pps)||
      read_turf_reg(daq, &turf_time.last_dead, &st->last_dead)||
      read_turf_reg(daq, &turf_time.llast_dead, &st->llast_dead)||
      read_turf_reg(daq, &turf_time.panic_counter, &st->panic_count)||
      read_turf_reg(daq, &turf_event.count_reg, &counts.as_uint)||
      read_turf_reg(daq, &turf_trig.holdoff_reg, &holdoff.as_uint)||
      read_turf_reg(daq, &turf_trig.running, &running)||
      read_turf_reg(daq, &turf_trig.offset, &offset)||
      read_turf_reg(daq, &turf_trig.latency, &latency)||
      read_turf_reg(daq, &turf_event.event_in_reset, &in_reset)||
      read_turf_reg(daq, &turf_event.full_error0, &st->full_err[0])||
      read_turf_reg(daq, &turf_event.full_error1, &st->full_err[1])||
      read_turf_reg(daq, &turf_event.full_error2, &st->full_err[2])||
      read_turf_reg(daq, &turf_trig.pps_reg, &pps_reg) ||
      read_turf_reg(daq, &turf_trig.photo_reg, &photoshutter_reg) ||
      read_turf_reg(daq, &turf_trig.mask, &st->trigger_mask)
     )
  {
    return 1;
  }

  st->pps_trig_enabled = pps_reg &1;
  st->pps_trig_offset = pps_reg >> 16;

  st->offset = offset;
  st->ack_count = counts.as_count.ack_count;
  st->allow_count = counts.as_count.allow_count;
  st->running = running;
  st->holdoff =  holdoff.as_holdoff.holdoff;
  st->turf_err = holdoff.as_holdoff.turf_err;
  st->latency = latency;
  st->surf_err = holdoff.as_holdoff.surf_err;
  st->leveltwo_logic = holdoff.as_holdoff.leveltwo_logic;
  st->rf_trig_en = holdoff.as_holdoff.rf_trig_en;
  st->in_reset = in_reset;
  st->photoshutter_enabled = !!(photoshutter_reg & (1 <<16));
  st->photoshutter_prescale = photoshutter_reg & 0xff;

  return 0;
}

int pueo_daq_get_scalers(pueo_daq_t * daq, pueo_daq_scalers_t* s)
{
  if (!s) return -1;


  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  if (daq->cfg.max_age > 0 && DELTA_T(now, daq->cached_scalers.readout_time)  < daq->cfg.max_age)
  {
    memcpy(s,&daq->cached_scalers, sizeof(*s));
    return 0;
  }
  memcpy(&s->readout_time, &now, sizeof(now));

  int ret =  read_incrementing_regs(daq, 32, 0, &turf_scalers.scaler_base, s->scalers.v);

  if (ret) return ret;

  // we now have a few extra. If I were smart, I'd read the whole register space at once
  uint32_t the_rest_of_the_scalers[3];

  ret = read_incrementing_regs(daq, 3, 0, &turf_scalers.mie_hvscaler,the_rest_of_the_scalers);

  //use memcpy to avoid alignment problems
  memcpy(&s->MIE_total_H, the_rest_of_the_scalers, sizeof(the_rest_of_the_scalers));


  if (ret) return ret; 

  if (daq->cfg.max_age)
  {
    memcpy(&daq->cached_scalers, s, sizeof(*s));
  }
  return ret;
}

int pueo_daq_read_L2_stat(pueo_daq_t * daq, pueo_L2_stat_t* s)
{
  if (!s) return -1;

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  if (daq->cfg.max_age > 0 && DELTA_T(now, daq->cached_l2.readout_time_start)  < daq->cfg.max_age)
  {
    memcpy(s,&daq->cached_l2, sizeof(*s));
    return 0;
  }

  memcpy(&s->readout_time_start, &now, sizeof(now));

  int ret =  read_incrementing_regs(daq, 24, 0, &turf_scalers.leveltwo_base, (uint32_t*) s->Hscalers);

  if (daq->cfg.max_age)
  {
    memcpy(&daq->cached_l2, s, sizeof(*s));
  }
  return ret;
  /*  non incrementing version here 
  reg_t reg = turf_scalers.leveltwo_base;
  for (int i = 0; i < 24; i++)
  {
    if (read_turf_reg(daq, &reg, &s->Hscalers[i])) return -1;
    reg.addr+=4;
  }
  */

}



int pueo_daq_scalers_dump(FILE *f, const pueo_daq_scalers_t * s)
{
  int ret = fprintf(f,"TURF Scalers @ %lu.%09lu\n", s->readout_time.tv_sec, s->readout_time.tv_nsec);
  ret +=    fprintf(f,"   soft: %hu,  pps: %hu,  ext: %hu,  reserved: %hu\n", s->scalers.map.soft, s->scalers.map.pps, s->scalers.map.ext, s->scalers.map.reserved);
  for (int i = 0; i < 4; i++)
  {
    const uint32_t *v = i == 0 ? s->scalers.map.turfio0_slots :
                  i == 1 ? s->scalers.map.turfio1_slots :
                  i == 2 ? s->scalers.map.turfio2_slots :
                           s->scalers.map.turfio3_slots ;
    ret += fprintf(f,"   TFIO%d: %u %u %u %u %u %u %u\n", i,
                          v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
  }

  ret += fprintf(f,  "*** MIE Totals  H/V/Sum: %hu/%hu/%hu\n", s->MIE_total_H, s->MIE_total_V, s->MIE_total_H + s->MIE_total_V);
  ret +=  fprintf(f,  "    LF Totals H/V/Sum: %hu/%hu/%hu\n",  s->LF_total_H, s->LF_total_V, s->LF_total_H + s->LF_total_V);
  ret +=  fprintf(f,  "    Aux/Global: %hu/%hu\n",  s->aux_total, s->global_total);

  return ret;

}

int pueo_daq_pps_setup(pueo_daq_t *daq, bool enable, uint16_t offset)
{

  if (write_turf_reg(daq, &turf_trig.pps_reg, 0)) return 1;
  //set the oofset
  uint32_t reg = offset;
  reg<<=16;
  if (write_turf_reg(daq, &turf_trig.pps_reg, reg )) return 1;
  if (enable)
  {
    usleep(10);
    reg|=1;
    if (write_turf_reg(daq, &turf_trig.pps_reg, reg)) return 1;
  }

  return 0;
}

int pueo_daq_L1_stat_dump(FILE *f, const pueo_L1_stat_t * s)
{
   int ret = 0;
   ret+= fprintf(f,"L1 stat dump for SURF Link %u, slot %u @%lu.%09ld (dur %hu ms), surf_holdoff: %x:\n" , s->surf_link, s->surf_slot, s->readout_time_start.tv_sec, s->readout_time_start.tv_nsec, s->ms_elapsed, s->surf_holdoff);
   ret+= fprintf(f,"==BM=====THRESHOLD/PSEUDOTHRESHOLD====== SCALER/PSEUDOSCALER====INMASK==BANK\n");
   for (int i = 0; i < PUEO_L1_BEAMS; i++)
   {
      ret+= fprintf(f,"  %02d    %06d/%06d         %5d/%5d          %c   %hhu:%hhu\n", i, s->beams[i].threshold, s->beams[i].pseudothreshold, s->beams[i].scaler, s->beams[i].pseudoscaler, s->beams[i].in_beam_mask ? 'x' : ' ', (uint8_t) s->beams[i].scaler_bank_before,(uint8_t) s->beams[i].scaler_bank_after);
   }

   ret += fprintf(f,"AGC SCALE/OFFS: ");
   for (int i = 0; i < PUEO_NSURF_CHAN; i++)
   {
     ret+= fprintf(f,"[%d: %d/%d  ] ", i,  s->agc_scale[i], s->agc_offset[i]);
   }
   fprintf(f,"\n");
   return ret;
}

int pueo_daq_L2_stat_dump(FILE *f, const pueo_L2_stat_t * s)
{
   int ret = 0;
   uint32_t hsum = 0, vsum = 0;
   ret+= fprintf(f,"L2 stat dump @%lu.%09ld\n" ,s->readout_time_start.tv_sec, s->readout_time_start.tv_nsec);
   ret+= fprintf(f,"==SEMISECTOR=====HSCAL======VSCAL==\n");
   for (int i = 0; i < 12; i++)
   {
      ret+= fprintf(f,"  %02d    %06d    %06d\n ", i, s->Hscalers[i], s->Vscalers[i]);
      hsum += s->Hscalers[i];
      vsum += s->Vscalers[i];
   }
   ret += fprintf(f,"==TOTALS:  H: %u, V: %u, all: %u\n", hsum, vsum, hsum+vsum);
   return ret;
}


#define PUEODAQ_L1_NOMULTIWRITE
int pueo_daq_set_L1_thresholds(pueo_daq_t * daq, int surf_link, int surf_slot, const uint32_t * thresholds, const uint32_t * pseudo_thresholds)
{

  if (thresholds)
  {
#ifndef PUEODAQ_L1_NOMULTIWRITE
    if ( write_incrementing_regs(daq, PUEO_L1_BEAMS, SURF_BASE(surf_link, surf_slot), &surf_L1.threshold_base, thresholds))
      return 1;
#else
    reg_t thresh_reg = surf_L1.threshold_base;
    for (int ibeam = 0; ibeam < PUEO_L1_BEAMS; ibeam++)
    {
      write_surf_reg(daq, SURF(surf_link,surf_slot), &thresh_reg, thresholds[ibeam]);
      thresh_reg.addr += 4;
    }
#endif


  }

  if (pseudo_thresholds)
  {

#ifndef PUEODAQ_L1_NOMULTIWRITE
    if ( write_incrementing_regs(daq, PUEO_L1_BEAMS, SURF_BASE(surf_link, surf_slot), &surf_L1.pseudothreshold_base, pseudo_thresholds))
      return 1;

#else
    reg_t thresh_reg = surf_L1.pseudothreshold_base;
    for (int ibeam = 0; ibeam < PUEO_L1_BEAMS; ibeam++)
    {
      write_surf_reg(daq, SURF(surf_link,surf_slot), &thresh_reg, pseudo_thresholds[ibeam]);
      thresh_reg.addr += 4;
    }
#endif

    write_surf_reg(daq, SURF(surf_link,surf_slot), &surf_L1.threshold_update_request, 1);

  }
  return 0;
}

int pueo_daq_set_L1_masks(pueo_daq_t * daq, int link, int slot, uint64_t beam_mask)
{
  return
  write_surf_reg(daq, SURF(link,slot), &surf_L1.lower_beam_mask, (~beam_mask) &0x3ffff) ||
  write_surf_reg(daq, SURF(link,slot), &surf_L1.upper_beam_mask, (1ull << 31) | (((~beam_mask) >> 18) & 0x3fffffff));
}


#ifndef PUEODAQ_L1_NOMULTIREAD
int pueo_daq_read_L1_stat(pueo_daq_t * daq, int link, int slot, pueo_L1_stat_t * stat)
{
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  if (daq->cfg.max_age > 0 && DELTA_T(now, daq->cached_l1[link][slot].readout_time_start)  < daq->cfg.max_age)
  {
    memcpy(stat,&daq->cached_l1[link][slot], sizeof(*stat));
    return 0;
  }

  //check if the SURF exists
  if (!daq->census.turfio[link].surfid[slot])
  {
    memset(stat,0,sizeof(*stat));
    return 0;
  }

  surf_t the_surf = SURF(link,slot);
  int r = 0;
  int i = 0;
  memcpy(&stat->readout_time_start, &now, sizeof(now));
  stat->surf_link = link;
  stat->surf_slot = slot;


  uint32_t lower_mask = 0;
  uint32_t upper_mask = 0;

  if (read_surf_reg(daq, the_surf, &surf_L1.lower_beam_mask, &lower_mask)) { r =1; goto do_end ;}
  if (read_surf_reg(daq, the_surf, &surf_L1.upper_beam_mask, &upper_mask)) { r =1; goto do_end ;}

  struct timespec end;
  uint64_t complete_mask = upper_mask & 0x3fffffff;
  complete_mask <<=18;
  complete_mask |= (lower_mask  & 0x3ffff);

  static __thread uint32_t thresholds[PUEO_L1_BEAMS];
  static __thread uint32_t pseudo_thresholds[PUEO_L1_BEAMS];
  static __thread uint16_t scalers[PUEO_L1_BEAMS][2];

  uint32_t scaler_bank[2];



  if (read_incrementing_regs(daq, PUEO_L1_BEAMS, SURF_BASE(the_surf.link, the_surf.slot),   &surf_L1.threshold_base, thresholds)) { r = 1; goto do_end;}
  if (read_incrementing_regs(daq, PUEO_L1_BEAMS, SURF_BASE(the_surf.link, the_surf.slot),   &surf_L1.pseudothreshold_base, pseudo_thresholds)) { r = 1; goto do_end;}
  if (read_surf_reg(daq, the_surf, &surf_L1.current_scaler_bank, &scaler_bank[0])) { r = 1; goto do_end; }
  if (read_incrementing_regs(daq, PUEO_L1_BEAMS, SURF_BASE(the_surf.link, the_surf.slot),   &surf_L1.scaler_base, (uint32_t*) scalers)) { r = 1; goto do_end;}
  if (read_surf_reg(daq, the_surf, &surf_L1.current_scaler_bank, &scaler_bank[1])) { r = 1; goto do_end; }

  for (int i = 0; i < PUEO_NSURF_CHAN; i++)
  {
    uint32_t scale, offset;
    read_based_reg(daq, SURF_BASE(the_surf.link, the_surf.slot)  + i * 1024, &surf_agc.scale, &scale);
    read_based_reg(daq, SURF_BASE(the_surf.link, the_surf.slot)  + i * 1024, &surf_agc.offset, &offset);
    stat->agc_scale[i] = scale;
    stat->agc_offset[i] = offset;
  }


  for (i = 0; i < PUEO_L1_BEAMS; i++)
  {

    stat->beams[i].threshold = thresholds[i];
    stat->beams[i].pseudothreshold = pseudo_thresholds[i];
    stat->beams[i].scaler = scalers[i][0];
    stat->beams[i].pseudoscaler = scalers[i][1];
    stat->beams[i].scaler_bank_before = scaler_bank[0];
    stat->beams[i].scaler_bank_after = scaler_bank[1];
    stat->beams[i].in_beam_mask = !( complete_mask & (1ull << i));
  }

  if (read_based_reg(daq, SURF_BASE(link,slot), &surf.surf_holdoff, &stat->surf_holdoff))
  {
    r = 1;
    goto do_end;
  }

do_end:

  clock_gettime(CLOCK_REALTIME, &end);
  stat->ms_elapsed = 1e3 * ( end.tv_sec - stat->readout_time_start.tv_sec + 1e-9 * (end.tv_nsec - stat->readout_time_start.tv_nsec));
  if (r)
  {
    stat->flags = 0xff;
    fprintf(stderr,"problem in pueo_daq_read_L1_stat (%d)\n", r);
  }
  else if (daq->cfg.max_age)
  {
    memcpy(&daq->cached_l1[link][slot], stat, sizeof(*stat));
  }


  return r;
}

#else

int pueo_daq_read_L1_stat(pueo_daq_t * daq, int link, int slot, pueo_L1_stat_t * stat)

{

  surf_t surf = SURF(link,slot);
  int r = 0;
  int i = 0;
  reg_t threshold_reg = surf_L1.threshold_base;
  reg_t pseudothreshold_reg = surf_L1.pseudothreshold_base;
  reg_t scaler_reg = surf_L1.scaler_base;
  clock_gettime(CLOCK_REALTIME, &stat->readout_time_start);
  stat->surf_link = link;
  stat->surf_slot = slot;

  uint32_t lower_mask = 0;
  uint32_t upper_mask = 0;

  if (read_surf_reg(daq, surf, &surf_L1.lower_beam_mask, &lower_mask)) { r =1; goto do_end ;}
  if (read_surf_reg(daq, surf, &surf_L1.upper_beam_mask, &upper_mask)) { r =1; goto do_end ;}

  struct timespec end;
  uint64_t complete_mask = upper_mask & 0x3fffffff;
  complete_mask <<=18;
  complete_mask |= (lower_mask  & 0x3ffff);

  for (i = 0; i < PUEO_L1_BEAMS; i++)
  {

    uint32_t thresh = 0;
    uint32_t pseudo_thresh = 0;
    uint16_t scalers[2] = {0};
    uint32_t scaler_bank[2];

    if (read_surf_reg(daq, surf, &threshold_reg, &thresh)) { r = 1+i; goto do_end; }
    if (read_surf_reg(daq, surf, &pseudothreshold_reg, &pseudo_thresh)) { r = 1+i; goto do_end; }
    if (read_surf_reg(daq, surf, &surf_L1.current_scaler_bank, &scaler_bank[0])) { r = 1+i; goto do_end; }
    if (read_surf_reg(daq, surf, &scaler_reg, (uint32_t*) scalers)) { r = 1+i; goto do_end; }
    if (read_surf_reg(daq, surf, &surf_L1.current_scaler_bank, &scaler_bank[1])) { r = 1+i; goto do_end; }


    stat->beams[i].threshold = thresh;
    stat->beams[i].pseudothreshold = pseudo_thresh;
    stat->beams[i].scaler = scalers[0];
    stat->beams[i].pseudoscaler = scalers[1];
    stat->beams[i].scaler_bank_before = scaler_bank[0];
    stat->beams[i].scaler_bank_after = scaler_bank[1];
    stat->beams[i].in_beam_mask = !! ( complete_mask & (1ull << i));


    threshold_reg.addr+=4;
    pseudothreshold_reg.addr+=4;
    scaler_reg.addr+=4;
  }

do_end:



  clock_gettime(CLOCK_REALTIME, &end);
  stat->ms_elapsed = 1e3 * ( end.tv_sec - stat->readout_time_start.tv_sec + 1e-9 * (end.tv_nsec - stat->readout_time_start.tv_nsec));
  if (r)
  {
    stat->flags = 0xff;
    fprintf(stderr,"problem in pueo_daq_read_L1_stat (%d)\n", r);
  }

  return r;
}

#endif


int pueo_daq_set_L2_mask(pueo_daq_t * daq, uint32_t mask)
{
  mask=~mask; // TURF mask is inverted compared to what I hthink of it as
  mask&=0xcffffff;
  return write_turf_reg(daq, &turf_trig.mask, mask);
}

int pueo_daq_set_L2_mask_by_2phi(pueo_daq_t * daq, uint16_t H, uint16_t V)
{
  // MI
  uint32_t mask = (H & 0xfff) | ((V & 0xfff) <<12);

  //LF
  mask |= (!!(H & 0x1000)) << 26;
  mask |= (!!(V & 0x1000)) << 27;

  return pueo_daq_set_L2_mask(daq, mask);
}



int pueo_daq_setup_photoshutter(pueo_daq_t * daq, bool enable, uint8_t prescale)
{
  uint32_t reg = prescale;
  if (enable) reg |= (1 <<16);
  return write_turf_reg(daq, &turf_trig.photo_reg, reg);
}


int pueo_daq_bypass_all_biquads(pueo_daq_t * daq, int ibq)
{
  for (int itfio = 0 ; itfio < 4; itfio++)
  {
    for (int isurf = 0; isurf < 6; isurf++)
    {
      if (daq->census.turfio[itfio].surfid[isurf] == 0)
      {
        continue;
      }
      for (int chan = 0; chan < 8; chan++)
      {
        if (pueo_daq_bypass_biquad(daq, itfio, isurf, chan, ibq))
        {
          fprintf(stderr,"Failed to bypass byquad on link %d, slot %d, chan %d, ibq %d\n", itfio, isurf, chan, ibq);
          return 1;
        }
      }
    }
  }
  return 0;
}


int pueo_daq_disable_channel_from_L1(pueo_daq_t *daq, int link, int slot, int channel, bool disable)
{
  return write_based_reg(daq, SURF_BASE(link,slot) + channel * 1024, &surf_agc.channel_disable, disable ? 0: 1);
}

int pueo_daq_set_all_biquads(pueo_daq_t * daq, int ibq, const pueo_biquad_t * bq)
{
  for (int itfio = 0 ; itfio < 4; itfio++)
  {
    for (int isurf = 0; isurf < 6; isurf++)
    {
      if (daq->census.turfio[itfio].surfid[isurf] == 0)
      {
        continue;
      }
      for (int chan = 0; chan < 8; chan++)
      {
        if (pueo_daq_set_biquad(daq, itfio, isurf, chan, ibq, bq))
        {
          fprintf(stderr,"Failed to bypass byquad on link %d, slot %d, chan %d, ibq %d\n", itfio, isurf, chan, ibq);
          return 1;
        }
      }
    }
  }
  return 0;
}

int pueo_daq_set_surf_holdoff(pueo_daq_t *daq, int link, int slot, uint32_t holdoff)
{
  return write_based_reg(daq, SURF_BASE(link,slot), &surf.surf_holdoff, holdoff);
}



