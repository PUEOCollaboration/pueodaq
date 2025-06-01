#define _GNU_SOURCE

#include "turfeth.h"
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>

#include "pueodaq.h"
#include "pueodaq-net.h"

typedef enum
{
  TURF_PORT_READ_REQ = 0x5472,
  TURF_PORT_WRITE_REQ = 0x5477,
  TURF_PORT_CTL_REQ = 0x5463,
  TURF_PORT_ACK = 0x5461,
  TURF_PORT_NACK = 0x546e,
  TURF_PORT_FRAGMENT_MIN = 0x5400,
  TURF_PORT_FRAGMENT_MAX = 0x543f
} e_turf_ports;


typedef enum
{
  TURF_MAX_EVENT_SIZE = 1 << 20 ,
  TURF_MAX_FRAGMENTS = 1 << 10
}e_turf_limits;

typedef enum
{
  DAQ_MAX_READER_THREADS = 64
}e_daq_limits;



#define FRAGMENT_MAP_SZ 64
//event buffer implementation
struct event_buf
{
  //address for this buf (o the TURF)
  uint16_t address;

  //number of fragment expected
  uint16_t nfragments_expected;

  //number of fragments received
  _Atomic uint16_t nfragments_rcvd;

  //number of bytes expected
  uint32_t nbytes_expected;

  //bitmap of received fragments, up to maximum number of fragments
  //this is not in a standard order due to the way the way fragments are split amongst threads to avoid false sharing
  // the ith fragment goes to  (i % nrecv_threads)
  volatile uint16_t fragment_map[FRAGMENT_MAP_SZ];

  uint64_t event_number; // Sequential event number received

  uint32_t run_number; // Set from daq config
  //flexible array member!
  uint8_t bytes[];
};

struct turf_tx_ctx
{
  int sock;
  struct sockaddr addr;
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

    pthread_mutex_t tx_lock;
    struct sockaddr_in wr_address;
    struct sockaddr_in rd_address;

    uint8_t wr_tag : 4;
    uint8_t rd_tag : 4;


    //port to send from daq to turf
    int daq_tx_sck;

    // these are receiving ports on
    int daq_ctl_sck;
    int daq_frgctl_sck;
    int daq_frg_sck;

  } net;


  int evbuf_sz; // padded to nearest page... so not the same as cfg.max_ev_size

  // the event buffer, may be internal or external...
  uint8_t * evmem;
  uint8_t * evmem_internal;
  uint16_t n_event_bufs;

  uint32_t run_number;
  uint64_t event_counter;
  pthread_mutex_t counter_lock;

  uint32_t turfid;
  enum
  {
    PUEODAQ_UNINIT,
    PUEODAQ_IDLE,
    PUEODAQ_RUNNING,
    PUEODAQ_ERROR
  } state;
};

static inline struct event_buf * event_buf_get(pueo_daq_t * daq, int i)
{
  return (struct event_buf*)  (daq->evmem + i * daq->evbuf_sz);
}



static int event_buf_reset(struct event_buf * evbuf)
{
  evbuf->nfragments_expected = 0;
  evbuf->nfragments_rcvd = 0;
  evbuf->nbytes_expected = 0;
  evbuf->event_number = 0;
  evbuf->run_number = 0;

  for (int i = 0; i < FRAGMENT_MAP_SZ; i++) evbuf->fragment_map[i] = 0;
  // we can leave bytes alone...


  return 0;
}


static int pgsiz;
__attribute__((constructor))
static void set_pgsize()
{
  pgsiz = sysconf(_SC_PAGESIZE);
}


static inline int next_page(int len)
{
  return ((pgsiz-1) & len) ? ((len+pgsiz) & ~(pgsiz-1)) : len;
}


static const pueo_daq_config_t default_config = { PUEO_DAQ_CONFIG_DFLT } ;

size_t pueo_daq_get_event_size(const pueo_daq_t * daq)
{
  return daq->evbuf_sz;
}


int pueo_daq_set_buffer(pueo_daq_t * daq, size_t nbytes, void * buf)
{
  //allocate internal buffer
  if (!nbytes || !buf)
  {
    if (daq->cfg.n_event_bufs_to_alloc)
    {
      daq->evmem_internal = calloc(daq->cfg.n_event_bufs_to_alloc, daq->evbuf_sz);
      if (!daq->evmem_internal)
      {
        return -1;
      }

      daq->evmem = daq->evmem_internal;
      daq->n_event_bufs = daq->cfg.n_event_bufs_to_alloc;
    }
  }
  else
  {
    // calculate number of events
    size_t nevents = nbytes / daq->evbuf_sz;
    if (nevents > 65535) nevents = 65535; //  "only" support so many adresses.

    //too small...
    if(!nevents)
    {
      return -1;
    }

    //otherwise we'll allow it

    if (daq->evmem_internal)
    {
      free(daq->evmem_internal);
      daq->evmem_internal = 0;
    }


    daq->evmem = buf;
    daq->n_event_bufs = nevents;
  }

  //set addresses
  for (uint32_t i = 0; i < daq->n_event_bufs; i++)
  {
    event_buf_get(daq, i)->address = i;
  }

  return 0;

}

pueo_daq_t * pueo_daq_init(const pueo_daq_config_t * cfg)
{
  if (!cfg)
  {
    cfg = &default_config;
  }

  if (pueo_daq_config_validate(cfg,stderr))
  {
    fprintf(stderr,"pueo_daq_init: Configuration did not validate!\n");
    return NULL;
  }

  pueo_daq_t * daq = calloc(sizeof(pueo_daq_t),1);
  if (!daq)
  {
    fprintf(stderr,"Could not allocate memory for pueo_daq_t!!!");
    return 0;
  }


  //copy the config in
  memcpy(&daq->cfg, cfg, sizeof(pueo_daq_config_t));

  //set some defaults in config if necessary
  if (!daq->cfg.n_recvthreads) daq->cfg.n_recvthreads = default_config.n_recvthreads;


  //set up buffers
  daq->evbuf_sz = next_page(daq->cfg.max_ev_size + sizeof(struct event_buf)); //round to page size

  //allocate internal buffer (maybe)
  if (pueo_daq_set_buffer(daq, 0,0))
  {
      fprintf(stderr, "Could not allocate memory for event buffers!!!\n");
      goto bail;
  }

  //set addresses
  for (int i = 0; i < daq->evbuf_sz; i++)
  {
    event_buf_get(daq, i)->address = i;
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
#define SETUP_SOCK(which,port,flags) \
  do {\
    daq->net.which = socket(AF_INET, SOCK_DGRAM, flags);\
    if (daq->net.which < 0) { \
      fprintf(stderr, "Couldn't open socket %s\n", #which); \
      goto bail; \
    }\
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = port, .sin_addr = {.s_addr = htonl(INADDR_ANY)} }; \
    if (bind(daq->net.which,(struct sockaddr*) &addr, sizeof(addr))) { \
     fprintf (stderr,"Couldn't bind %s to %d\n", #which, port);\
     goto bail; \
    }\
  } while(0)


  SETUP_SOCK(daq_tx_sck, daq->cfg.daq_ports.tx_out,0);
  SETUP_SOCK(daq_ctl_sck, daq->cfg.daq_ports.control_in,0);
  SETUP_SOCK(daq_frgctl_sck, daq->cfg.daq_ports.fragcontrol_in,0);
  SETUP_SOCK(daq_frg_sck, daq->cfg.daq_ports.fragment_in,0);


  //set up dest addresses

  daq->net.wr_address = (struct sockaddr_in) {.sin_family = AF_INET, .sin_port = TURF_PORT_WRITE_REQ, .sin_addr = daq->net.turf_ip};
  daq->net.rd_address = (struct sockaddr_in) {.sin_family = AF_INET, .sin_port = TURF_PORT_READ_REQ, .sin_addr = daq->net.turf_ip};

  //set up locks
  pthread_mutex_init(&daq->net.tx_lock,0);
  pthread_mutex_init(&daq->counter_lock, 0);
  daq->run_number = 0;
  daq->event_number = 0;

  // reset tags on DAQ
  pueo_daq_read(daq,0,&daq->turfid);
  daq->state = PUEODAQ_IDLE;

  //setu p all acks

  return daq;

  // on failure, we clean up after ourselves like good citizens
bail:
  pueo_daq_destroy(daq);
  return 0;
}


void pueo_daq_destroy(pueo_daq_t * daq)
{
  if (!daq) return;

  //close sockets
  close(daq->net.daq_tx_sck);
  close(daq->net.daq_ctl_sck);
  close(daq->net.daq_frgctl_sck);
  close(daq->net.daq_frg_sck);

  //deallocate buffers, if they exis t
  if (daq->evmem_internal)
    free(daq->evmem_internal);

  //deallocate the daq
  free(daq);
}


struct blocking_wait_check
{
  uint16_t wanted_port;
  uint8_t wanted_tag;
  uint32_t wanted_addr;
  void *buf ;
  size_t len;
};

struct addrtag_pair
{
  uint32_t addr:28;
  uint32_t tag:4;
};

static int blocking_wait_for_response(pueo_daq_t * daq,  const struct blocking_wait_check * check)
{
  int sck = daq->net.daq_tx_sck;
  struct pollfd fd;
  fd.fd = sck;
  fd.events = POLLIN;
  int ready = ppoll (&fd, 1, &daq->cfg.timeout, NULL);
  if (!ready || !(fd.revents & POLLIN ))  return -1;

  //just wait, don't check
  if (!check) return 0;

  //otherwise read it into the buf and do the check
  struct  sockaddr_in  src;
  socklen_t srclen = sizeof(src);

  ssize_t r = recvfrom(sck, check->buf, check->len,0, &src, &srclen);

  //make sure we got something
  if ( r <= 0) return -2;

  //check to make sure the packet came from the turf
  if (src.sin_addr.s_addr != daq->net.turf_ip.s_addr) return -3;

  // if we have wanted port verification, check it
  if (check->wanted_port && src.sin_port != check->wanted_port) return -4;

  //check for tag mismatch
  struct addrtag_pair * addrtag = (struct addrtag_pair *) check->buf;
  if (check->wanted_tag < 16 && addrtag->tag != check->wanted_tag) return -5;
  if (check->wanted_addr < 0xffffffff &&  addrtag->addr != check->wanted_addr) return -6;

  return 0;
}


int pueo_daq_write(pueo_daq_t * daq, uint32_t wraddr, uint32_t data)
{
  pthread_mutex_lock(&daq->net.tx_lock);
  uint8_t tag = daq->net.wr_tag++;

  turf_wrreq_t msg= {.BIT={.ADDR = wraddr & 0x0fffffff, .TAG = tag, .WRDATA = data}};
  turf_wrresp_t resp;

  int success = 0;
  for (unsigned attempt = 0; attempt < daq->cfg.max_attempts; attempt++)
  {
    int sent = sendto(daq->net.daq_tx_sck, &msg, sizeof(msg), 0, &daq->net.wr_address, sizeof(daq->net.wr_address));
    if (sent != sizeof(msg))
    {
      fprintf(stderr,"Sending problem?\n");
      continue;
    }
    struct blocking_wait_check chk = { .wanted_tag = tag, .wanted_addr = wraddr, .wanted_port = TURF_PORT_WRITE_REQ, .buf = &resp, .len = sizeof(resp)};

    if (!blocking_wait_for_response(daq, &chk))
    {
      success = 1;
      break;
    }
  }

  pthread_mutex_unlock(&daq->net.tx_lock);

  return !success;

}

int pueo_daq_read(pueo_daq_t * daq, uint32_t rdaddr, uint32_t *data)
{
  pthread_mutex_lock(&daq->net.tx_lock);
  uint8_t tag = daq->net.rd_tag++;

  turf_rdreq_t msg= {.BIT={.ADDR = rdaddr & 0x0fffffff, .TAG = tag}};
  turf_rdresp_t resp;

  int success = 0;
  for (unsigned attempt = 0; attempt < daq->cfg.max_attempts; attempt++)
  {
    int sent = sendto(daq->net.daq_tx_sck, &msg, sizeof(msg), 0, &daq->net.rd_address, sizeof(daq->net.rd_address));
    if (sent != sizeof(msg))
    {
      fprintf(stderr,"Sending problem?\n");
      continue;
    }
    struct blocking_wait_check chk = { .wanted_tag = tag, .wanted_addr = rdaddr, .wanted_port = TURF_PORT_READ_REQ, .buf = &resp, .len = sizeof(resp)};

    if (!blocking_wait_for_response(daq, &chk))
    {
      success = 1;
      *data = resp.BIT.RDDATA;
      break;
    }
  }

  pthread_mutex_unlock(&daq->net.tx_lock);

  return !success;
}



