#define _GNU_SOURCE

#include "turfeth.h"
#include "turfregs.h"
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <assert.h>
#include <math.h>
#include <errno.h>
#include <stdatomic.h>
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


typedef enum
{
  TURF_MAX_EVENT_SIZE = 1 << 20 ,
  TURF_MAX_FRAGMENTS = 1 << 10,
  TURF_NUM_ADDR = 1 << 12,
  TURF_MAX_ACKS = 1

}e_turf_limits;

typedef enum
{
  DAQ_MAX_READER_THREADS = 64
}e_daq_limits;


struct fragment
{
  turf_fraghdr_t hd;
  int16_t buf[];
};


static int __attribute__((nonnull))
pueo_daq_read_reg(pueo_daq_t * daq, const turfreg_t * reg, uint32_t * val)
{

  int r = pueo_daq_read(daq, reg->addr, val);
  (*val) &=reg->mask;
  (*val)>>=reg->offs;
  return r;
}

static int  __attribute__((nonnull))
pueo_daq_write_reg(pueo_daq_t * daq, const turfreg_t * reg, uint32_t val)
{
  assert (!reg->ro);
  val <<= reg->offs;
  val &= reg->mask;
  if (reg->len!=32)
  {
    uint32_t current;
    if (pueo_daq_read_reg(daq, reg, &current))
    {
      fprintf(stderr,"Could not read old reg\n");
      return -1;
    }
    val |= current & (~reg->mask);
  }

  return pueo_daq_write(daq, reg->addr, val);
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


//event buffer implementation
struct event_buf
{

  _Atomic(uint64_t) first_fragment_packed_time;
  _Atomic(uint64_t) last_fragment_packed_time;

  uint64_t event_number; // Sequential event number received
                         //
  uint32_t run_number; // Set from daq config
                         //
  //number of bytes expected
  uint32_t nbytes_expected;

  uint32_t header_size;
  uint32_t nsamples;

  //address for this buf (on the TURF)
  uint16_t address;

  //number of fragment expected
  volatile _Atomic(uint16_t) nfragments_expected;

  //number of fragments received
  volatile _Atomic(uint16_t) nfragments_rcvd;



  //list of fragment indices, this will be ceil(max_event_size / fragment_size) big
  uint32_t fragments[];
};


struct reader_thread_setup
{
  uint16_t tnum;
  pueo_daq_t * daq;
};


#define DFLT_RECV_THREADS 4
struct pueo_daq
{
  pueo_daq_config_t cfg; //The configuration, copied in (with default values populated if necessary)
  //networking stuff
  struct
  {
    struct in_addr turf_ip;
    struct in_addr turf_mask;
    struct in_addr our_ip;

    pthread_mutex_t tx_lock;

    uint8_t wr_tag : 4;
    uint8_t rd_tag : 4;


    // these are receiving ports on
    int daq_ctl_sck;
    int daq_frgctl_sck;
    int daq_frg_sck[DAQ_MAX_READER_THREADS];

  } net;

  struct
  {
    uint32_t turfid;
    struct
    {
      uint32_t turfioid;
      uint32_t surfid[7];
    } turfio;
  } census;


  // the event buffer
  uint16_t evbuf_sz;
  struct event_buf * event_bufs;

  // the fragment used map. bit is 1 if a fragment has been claimed
  atomic_uint_fast64_t *fragments_bitmap;
  uint32_t fragments_bitmap_size;//size of the bitmap
  // the fragment buffer.
  struct fragment * fragments;
  uint32_t nfragments;

  uint32_t run_number;

  pueo_daq_event_ready_callback_t cb;

  // This is incremented whenever we ack with an allow bit
  // It is only touched by the control thread after initialization
  volatile uint32_t num_events_allowed;

  // This is inrememented whenever a new address is encountered
  // This is atomic since it can be updated by any readout thread
  volatile _Atomic (uint32_t) num_events_discovered;

  //This is incremented when the last fragment of an event is found
  //Tihs is atomic since it can be updated by any readout thread
  volatile _Atomic (uint32_t) num_events_received;

  // This is incremented whenever we start giving out an event
  // This is atomic since readable via API
  volatile _Atomic (uint32_t) num_events_dispense_began;

  // This is incremented whenever we finish giving out an event
  // This is atomic since is read by the contorl thread and modifiable via API
  volatile _Atomic (uint32_t) num_events_dispensed;

  // This is incremented whenver an address gets assigned an index
  // This should only happen in the control thread after initialization
  volatile uint32_t num_addr_assigned;

  // This is basically the same as num_events_allowed, except for an offset...
  volatile uint32_t num_acks_sent;

  // The reader threads
  pthread_t reader_threads[DAQ_MAX_READER_THREADS];

  // The control thread (acks and nacks and address assignment)
  pthread_t ctl_thread;

  /*
  * This gives us the address map to event number.
  *
  * This gets updated at the beginning and we ack an event by the ctl thread
  */
  volatile uint32_t addr_map[TURF_NUM_ADDR];

  //A bit is set when we can ack an address (i.e. that transfer was completed).
  //This is written to by reader threads and cleared by ctl thread;
  volatile _Atomic(uint64_t) ack_map [ TURF_NUM_ADDR / 64];


  // The DAQ state.
  volatile _Atomic enum
  {
    PUEODAQ_UNINIT,
    PUEODAQ_IDLE,
    PUEODAQ_STARTING,
    PUEODAQ_RUNNING,
    PUEODAQ_ERROR,
    PUEODAQ_STOPPING
  } state;

};


struct blocking_wait_check
{
  uint64_t wanted;
  uint64_t wanted_mask;
  uint64_t val;
};

#define READ_WAIT_CHECK(addr,tag) ( (struct blocking_wait_check) {.wanted = (addr & 0x0fffffff) | (tag << 28), .wanted_mask = 0xffffffff } )
#define WRITE_WAIT_CHECK(addr,tag)( (struct blocking_wait_check) {.wanted = (addr & 0x0fffffff) | (tag << 28), .wanted_mask = 0xffffffff } )
#define ACK_WAIT_CHECK(addr,tag,allow) ( (struct blocking_wait_check) { .wanted = (  (((uint64_t) allow) << 63) | (1ull << 62) | ( ((uint64_t) addr) <<20) | (((uint64_t)tag) << 32)), .wanted_mask = 0xc00000fffff00000} )
#define CTL_WAIT_CHECK(ctl) ( ( struct blocking_wait_check)  {.wanted = ctl.RAW, .wanted_mask = 0xffffffffffff0000 } )
#define PERMISSIVE_WAIT_CHECK() ( (struct blocking_wait_check)  { .wanted_mask = 0 } )


static void * control_thread(void * arg);
static void * reader_thread(void * arg);



static int blocking_wait_for_response(pueo_daq_t * daq,  int sck, struct sockaddr_in *wr, struct blocking_wait_check * check)
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

  //just wait, don't check
  if (!check) return 0;

  //otherwise read it into the buf and do the check
  struct  sockaddr_in  src;
  socklen_t srclen = sizeof(src);

  ssize_t r = recvfrom(sck, &check->val, sizeof(check->val),0, &src, &srclen);

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

  //check for wanted mismatch
  if ( (check->wanted & check->wanted_mask) != (check->val & check->wanted_mask)) 
  {
    if (daq->cfg.debug) fprintf(stderr,"bwait: wanted fail. Got 0x%016lx, wanted 0x%016lx (mask 0x%016lx)\n", check->val, check->wanted, check->wanted_mask);
    return -5;
  }

  return 0;
}

#define LAUNDER_U64(x) *((uint64_t*) &x)

typedef union acked_msg
{
  uint64_t u;
  turf_ctl_t c;
  turf_rdreq_t r;
  turf_rdresp_t p;
  turf_wrreq_t w;
  turf_ack_t a;
} acked_msg_t __attribute__((__transparent_union__));

typedef union acked_msg_ptr
{
  uint64_t *u;
  turf_ctl_t *c;
  turf_rdreq_t *r;
  turf_rdresp_t *p;
  turf_wrreq_t *w;
  turf_ack_t *a;
  acked_msg_t * m;
} acked_msg_ptr_t __attribute__((__transparent_union__));



static_assert(sizeof(acked_msg_t) == sizeof(uint64_t));

static int acked_multisend(pueo_daq_t * daq, int sock, uint16_t port, size_t Nsend,
    acked_msg_ptr_t snd, acked_msg_ptr_t rcv, struct blocking_wait_check * check)
{

  if (daq->cfg.debug)
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
    ssize_t sent = sendto(sock, snd.m, Nsend * sizeof(acked_msg_t), 0, (struct sockaddr*) &a, sizeof(a));
    if (sent != (ssize_t) (Nsend *sizeof(acked_msg_t)))
    {
      fprintf(stderr,"Sending problem?\n");
      continue;
    }

    if (!check) break;
    if (!blocking_wait_for_response(daq, sock, &a, check))
    {
      if (daq->cfg.debug) 
      {
        printf( "<   0x%016lx\n", check->val);
      }
      if (rcv.u) *rcv.u= check->val;
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
  return (struct event_buf*)  (daq->event_bufs + i * daq->evbuf_sz);
}

static inline struct fragment * fragment_get(pueo_daq_t * daq, uint32_t i)
{
  return daq->fragments + i * (sizeof(turf_fraghdr_t) + daq->cfg.fragment_size);
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
  int b = addr & 64;
  uint64_t m = 1ull << b;

  uint64_t was = atomic_fetch_and(&daq->ack_map[w], ~m);

  // check to make sure we're not double acking.
  assert (!(was  & ~m));
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
    uint64_t was = atomic_load(&daq->fragments_bitmap[word]);
    while (was != ~(0ull))
    {
      //this has to exist since we already asserted was is not full
      int first_free_bit = __builtin_ctzll(~was);

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

static struct  event_buf * event_buf_for_addr(pueo_daq_t * daq, uint16_t addr)
{
  // The index of this address
  uint32_t evnum = daq->addr_map[addr];
  uint16_t ev_index = evnum % daq->cfg.n_event_bufs;
  return event_buf_get(daq,ev_index);
}


static int event_buf_reset(struct event_buf * evbuf)
{
  // this should only happen in a single thread at a time so no need to use atomic stores
  evbuf->nfragments_expected = 0;
  evbuf->nfragments_rcvd = 0;
  evbuf->nbytes_expected = 0;
  evbuf->nsamples = 0;
  evbuf->header_size = 0;
  evbuf->event_number = 0;
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
      daq->nfragments * (sizeof(turf_fraghdr_t) + daq->cfg.fragment_size),
      PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1,0);

  daq->fragments_bitmap = calloc(daq->fragments_bitmap_size, sizeof(*daq->fragments_bitmap));

  if (! daq->fragments || !daq->fragments_bitmap)
  {
    fprintf(stderr,"Couldn't allocate fragments :( %s\n", strerror(errno));
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


  daq->evbuf_sz = sizeof(struct event_buf) + ceil(1.0*daq->cfg.max_ev_size / daq->cfg.fragment_size);

  errno=0;
  daq->event_bufs = mmap( NULL, daq->cfg.n_event_bufs * daq->evbuf_sz,
                          PROT_READ | PROT_WRITE, MAP_ANONYMOUS, -1, 0);

  if (! daq->event_bufs)
  {
    fprintf(stderr,"Couldn't mmap event_bufs :( %s\n", strerror(errno));
    goto bail;

  }

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
#define SETUP_SOCK(which,port,opts,dest_port) \
  do {\
    daq->net.which = socket(AF_INET, SOCK_DGRAM, 0);\
    if (daq->net.which < 0) { \
      fprintf(stderr, "Couldn't open socket %s\n", #which); \
      goto bail; \
    }\
    if (opts != NULL) { \
      int iopt = 0; int optval = 1;\
      while( ((int*) opts)[iopt])\
      {\
        if (setsockopt(daq->net.which, SOL_SOCKET, ((int*)opts)[iopt++], &optval, sizeof(optval)))\
        {\
          fprintf(stderr,"Couldn't setsockopt :(\n"); goto bail;\
        }\
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


  SETUP_SOCK(daq_ctl_sck, daq->cfg.daq_ports.control_in,0,0);
  SETUP_SOCK(daq_frgctl_sck, daq->cfg.daq_ports.fragcontrol_in,0,0);

  for (unsigned i = 0; i < daq->cfg.n_recvthreads; i++ )
  {
    //TODO: we should protect against multiple instances here by using some sort of external lock on the port
    int opts[] = { daq->cfg.n_recvthreads == 1 ? 0 : SO_REUSEPORT, 0 };
    SETUP_SOCK(daq_frg_sck[i], daq->cfg.daq_ports.fragment_in,opts,0);
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
    struct reader_thread_setup s = {.tnum = i, .daq = daq };
    if (pthread_create(&daq->reader_threads[i], NULL, reader_thread, &s))
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

  // open the event interface
  turf_ctl_t ctl  = {.BIT =
    {
      .COMMAND = TURF_OP_COMMAND,
      .PAYLOAD = ((uint64_t)htons(daq->cfg.daq_ports.fragment_in)) | ( ((uint64_t)htonl(daq->net.our_ip.s_addr)) << 16)
    }
  };

  if (acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl, NULL, &CTL_WAIT_CHECK(ctl)))
  {
    fprintf(stderr,"Problem with open\n");
    return 1;
  }


  // read and write the bit
  uint32_t reset = 0;
  if (pueo_daq_read_reg(daq, &turf_event.reset_events, &reset))
  {
    fprintf(stderr,"Could not read reset_events\n");
    return 1;
  }
  if (!reset)
  {
    if (pueo_daq_write_reg(daq, &turf_event.reset_events, 1)
        || pueo_daq_write_reg(daq, &turf_event.reset_events, 0))
    {
      fprintf(stderr,"Could not write reset_events\n");

      return 1;
    }

  }
  else {
    printf("Already in reset???\n");
  }

  //setup all acks, batch in groups of TURF_MAX_ACKS
  int nallow = daq->cfg.max_in_flight;
  daq->num_events_allowed = nallow;
  for (unsigned i = 0; i < TURF_NUM_ADDR; i+=TURF_MAX_ACKS)
  {
    turf_ack_t acks[TURF_MAX_ACKS];
    for (unsigned j = 0 ; j < TURF_MAX_ACKS; j++)
    {
      acks[j].BIT.ADDR = i+j;
      acks[j].BIT.TAG = i + j;
      acks[j].BIT.ALLOW = !!(nallow-- > 0);
      daq->addr_map[i+j] = i+j;
    }

    if (acked_multisend(daq, daq->net.daq_frgctl_sck, TURF_PORT_ACK, TURF_MAX_ACKS, acks,
          NULL,  &ACK_WAIT_CHECK(acks[TURF_MAX_ACKS-1].BIT.ADDR, acks[TURF_MAX_ACKS-1].BIT.TAG, acks[TURF_MAX_ACKS-1].BIT.ALLOW)))
    {
      fprintf(stderr,"Problem sending acks\n");
      return 1;
    }
  }

  daq->num_addr_assigned = TURF_NUM_ADDR;
  if (pueo_daq_write_reg(daq, &turf_trig.runcmd, RUNCMD_RESET))
  {
    fprintf(stderr,"Could not run runcmd\n");
    return 1;

  }
  atomic_store(&daq->state, PUEODAQ_RUNNING);

  return 0;
}

int pueo_daq_stop(pueo_daq_t * daq)
{
  int expected = PUEODAQ_RUNNING;
  atomic_compare_exchange_strong(&daq->state, &expected, PUEODAQ_IDLE);

  if (expected == PUEODAQ_RUNNING)
  {
    if (pueo_daq_write_reg(daq, &turf_trig.runcmd, RUNCMD_STOP))
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
  return acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl, NULL, &CTL_WAIT_CHECK(ctl));

  return 0;
}

int pueo_daq_reset(pueo_daq_t * daq)
{


  //If we have seen events before, we have to clear the event bufs
  if (atomic_load(&daq->num_events_discovered))
  {
    //make sure we're stopped
    pueo_daq_stop(daq);

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
  pueo_daq_read(daq,0,&daq->census.turfid);

  //TODO take a census of who we have



  // close the event interface
  pueo_daq_stop(daq);

  // setup fragments etc.
  // get max fragments

  turf_ctl_t ctl = { .BIT.COMMAND = TURF_PR_COMMAND };

  if (acked_send(daq,daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl, &ctl, &PERMISSIVE_WAIT_CHECK()))
  {
      fprintf(stderr,"Problem calling PR\n");
      return 1;
  }
  turf_ctl_param_t pr = {.RAW = ctl.BIT.PAYLOAD};
  printf("%hu %hu %hu\n", pr.BIT.FRAGSRCMASK, pr.BIT.ADDR, pr.BIT.FRAGMENT);

  /*
  turf_ctl_param_t p = {.BIT.FRAGSRCMASK = 0x3f, .BIT.ADDR = 4095, .BIT.FRAGMENT = daq->cfg.fragment_size-8};
  ctl.BIT.PAYLOAD =  p.RAW;
  ctl.BIT.COMMAND = TURF_PW_COMMAND;

  if (acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_CTL_REQ, ctl, NULL, &CTL_WAIT_CHECK(ctl)))
  {
      fprintf(stderr,"Problem calling PW\n");
      return 1;
  }

  */

  atomic_store(&daq->state,PUEODAQ_IDLE);

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
    pthread_cancel(daq->reader_threads[i]);
  }
  for (unsigned i = 0; i < daq->cfg.n_recvthreads; i++)
  {
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
    munmap(daq->fragments, daq->nfragments * (sizeof(turf_fraghdr_t) + daq->cfg.fragment_size));
  }

  if (daq->fragments_bitmap) free(daq->fragments_bitmap);


  //deallocate the daq
  free(daq);
}




int pueo_daq_write(pueo_daq_t * daq, uint32_t wraddr, uint32_t data)
{
  pthread_mutex_lock(&daq->net.tx_lock);
  uint8_t tag = daq->net.wr_tag++;
  turf_wrreq_t msg= {.BIT={.ADDR = wraddr & 0x0fffffff, .TAG = tag, .WRDATA = data}};
  int r = acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_READ_REQ, msg, NULL, &WRITE_WAIT_CHECK(wraddr, tag));
  pthread_mutex_unlock(&daq->net.tx_lock);

  return r;
}


int pueo_daq_read(pueo_daq_t * daq, uint32_t rdaddr, uint32_t *data)
{
  pthread_mutex_lock(&daq->net.tx_lock);
  uint8_t tag = daq->net.rd_tag++;
  turf_rdreq_t msg= {.BIT={.ADDR = rdaddr & 0x0fffffff, .TAG = tag}};
  turf_rdresp_t resp;
  int r = acked_send(daq, daq->net.daq_ctl_sck, TURF_PORT_READ_REQ, msg, &resp, &READ_WAIT_CHECK(rdaddr, tag));
  pthread_mutex_unlock(&daq->net.tx_lock);
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
    int state = atomic_load(&daq->state);
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
    struct sockaddr src;
    socklen_t src_len;
    //get the index of a free fragment
    uint16_t frag_i = fragment_find_free(daq, s->tnum);
    struct fragment * frag = fragment_get(daq, frag_i);

    errno = 0;
    ssize_t nrecv = recvfrom(sck_frg, frag, daq->cfg.fragment_size + sizeof(turf_fraghdr_t),0, &src, &src_len);

    if (nrecv  < 0) continue; //TODO do better, though often this is EINTR

    uint16_t fragnum = frag->hd.BIT.FRAGMENT;
    uint16_t addr = frag->hd.BIT.ADDR;
    //uint32_t kid = frag->hd.BIT.KID;
    uint32_t evlen = frag->hd.BIT.TOTAL;

    //this will update nfragments_expected (and nbytes expected if it's the first time we see this event
    struct event_buf * ev = event_buf_for_addr(daq, addr);

    struct timespec now;

    //set nfragments_expected if it hasn't been set already
    uint16_t zero = 0;
    if (atomic_compare_exchange_strong(&ev->nfragments_expected,&zero, (evlen + daq->cfg.fragment_size -1) / daq->cfg.fragment_size))
    {

      clock_gettime(CLOCK_MONOTONIC,&now);
      uint64_t packed_now = pack_time(now);

      //TODO are there race conditions here?
      ev->run_number = daq->run_number;
      ev->event_number = atomic_fetch_add(&daq->num_events_discovered,1);
      ev->nbytes_expected = evlen;
      //TODO: should we use SO_TIMESTAMP[NS]?
      ev->address = addr;
      atomic_store(&ev->first_fragment_packed_time, packed_now);
    }
    else
    {
      while(!atomic_load(&ev->first_fragment_packed_time)); // make sure the event is fully initialized
    }

    // store current time
    // this will be used by the nack detector
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t packed_now = pack_time(now);
    atomic_store(&ev->last_fragment_packed_time, packed_now);

    ev->fragments[fragnum] = frag_i;
    if (fragnum == 0)
    {
      ev->header_size =  (uint16_t) daq->fragments[frag_i].buf[0];
    }

    //add fragments received
    uint16_t prior = atomic_fetch_add(&ev->nfragments_rcvd, 1);

    // we finished.I think this is ok since we can't get more than nfagments_expected fragments, right?
    if (prior == ev->nfragments_expected -1)
    {
      uint32_t rcv_idx = atomic_fetch_add(&daq->num_events_received, 1);

      // schedule an ack if we have room for another event
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
#define ATOMIC_PRINT(x) r+= fprintf(stream,#x ": %u\n", atomic_load(&daq->x))
  ATOMIC_PRINT(num_events_discovered);
  ATOMIC_PRINT(num_events_received);
  ATOMIC_PRINT(num_events_dispensed);

  return r;
}

int pueo_daq_nready(const pueo_daq_t * daq)
{
  uint32_t rcv = atomic_load(&daq->num_events_received);
  uint32_t dis = atomic_load(&daq->num_events_dispensed);
  return rcv - dis;
}

void pueo_daq_wait_event(pueo_daq_t * daq)
{
  // TODO use a condition variable here? 
  // but that won't mesh well with our atomics design though...
  while (!pueo_daq_nready(daq)) usleep(500);
}

int pueo_daq_get_event(pueo_daq_t * daq, pueo_daq_event_data_t * dest, uint32_t nsample_capacity)
{
  pueo_daq_wait_event(daq);

  uint32_t started = atomic_fetch_add(&daq->num_events_dispense_began,1);
  struct event_buf * ev = event_buf_get(daq, started % daq->cfg.n_event_bufs);

  printf("Header size: %hu\n", ev->header_size);

  //header will always fit in first fragment
  memcpy(&dest->header, fragment_get(daq, ev->fragments[0])->buf, ev->header_size);


  uint32_t nsamples_readout = ev->nsamples;
  uint32_t last_fragment_size = ev->nbytes_expected % (daq->cfg.fragment_size + sizeof(turf_fraghdr_t));


  //TODO excise the non-existent surfs
  //TODO allow setting a mask
  //the happy case
  if (nsamples_readout <= nsample_capacity)
  {
    dest->nsamples_per_event = nsamples_readout;
    void * p = dest->waveform_data;
    p = mempcpy(p, fragment_get(daq,ev->fragments[0])->buf + ev->header_size, daq->cfg.fragment_size-ev->header_size);
    for (int i = 1; i < ev->nfragments_expected; i++)
    {
      p = mempcpy( p, fragment_get(daq,ev->fragments[i])->buf, i == ev->nfragments_expected - 1 ? last_fragment_size : daq->cfg.fragment_size);
    }
  }
  else if (nsample_capacity == 0)
  {
    //this is easy, we do nothing!
  }
  else // we only have room for truncated data. TODO this can probably be optimized
  {
    for (int i = 0; i < 224; i++)
    {
      int start_byte = ev->header_size + 2*nsamples_readout * i;
      int start_fragment = start_byte / daq->cfg.fragment_size;
      int end_fragment = (start_byte + nsample_capacity * 2 ) / daq->cfg.fragment_size;

      for (int f = start_fragment; f <= end_fragment; f++)
      {
        struct fragment * frag = fragment_get(daq,ev->fragments[f]);
        int start = f == start_fragment ? (start_byte % daq->cfg.fragment_size) : 0;
        void * p = dest->waveform_data;

        // we will never have to worry about the partial fragment since we're truncated anyways
        int end = f == end_fragment ? (start_byte+nsample_capacity * 2) % daq->cfg.fragment_size : daq->cfg.fragment_size;
        p = mempcpy(p, frag->buf + start, end-start);
      }
    }
  }


  //reset the event buf
  event_buf_reset(ev);

  //all done, we can release the event buf now
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

  // allow up to TURF_MAX_ACKS acqs at a time since why not

  turf_ack_t acks[TURF_MAX_ACKS];
  unsigned num_acks = 0;

  //more than one nack is overkill
  turf_nack_t nack;
  uint8_t tag = 0;


  while(true)
  {

    int state = atomic_load(&daq->state);
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
    uint32_t dispensed = atomic_load(&daq->num_events_dispensed);

    assert(dispensed <= daq->num_events_allowed);

    // we have room for more events if the number allowed minus the number
    // dispensed is less than to the number of event bufs we  have
    uint32_t capacity = daq->num_events_allowed - dispensed;

    if (capacity)
    {
      num_acks = 0;
      for (unsigned w = 0; w < sizeof(daq->ack_map) / sizeof(*daq->ack_map); w++)
      {
        if (num_acks >=TURF_MAX_ACKS || num_acks >= capacity) break;
        uint64_t word = atomic_load(&daq->ack_map[w]);

        if (word != 0) // we have a candidate to ack!
        {
          for ( int bit = 0; bit < 64; bit++)
          {
            if (num_acks >= TURF_MAX_ACKS || num_acks >= capacity) break;
            if (word & (1 << bit))
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

        //increment the addresses
        for (unsigned i = 0; i < num_acks; i++)
        {
          uint16_t addr = acks[num_acks-1].BIT.ADDR;
          daq->addr_map[addr] = daq->num_addr_assigned++;
        }
      }

    }

    // TODO go through and see if there is anything really old that needs to be nacked

  }
  return NULL;
}


int pueo_daq_soft_trig(pueo_daq_t * daq)
{
  return pueo_daq_write_reg(daq, &turf_trig.softrig, 1);
}

