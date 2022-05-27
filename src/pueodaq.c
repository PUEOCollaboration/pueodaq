#include "turfeth.h" 
#include <arpa/inet.h>
#include <netin/ip.h>
#include <net/if.h>
#include <sys/ioctl.h> 
#include <stdio.h> 


typedef enum
{
  TURF_PORT_READ_REQ = 21618, 
  TURF_PORT_WRITE_REQ = 21623, 
  TURF_PORT_CTL_REQ = 21603, 
  TURF_PORT_ACK = 21601, 
  TURF_PORT_NACK = 21614, 
  TURF_PORT_EVENT = 21605 
} e_turf_ports; 


typedef enum 
{ 
  TURF_MAX_EVENT_SIZE = 1 << 20 , 
  TURF_MAX_FRAGMENTS = 1 << 10 
}e_turf_limits;

typedef enum 
{
  DAQ_MAX_READER_THREADS = 16 

}e_daq_limits; 



//event buffer implementation 
struct event_buf  
{
  //address for this buf
  uint16_t address; 
  
  //number of fragment expected
  uint16_t nfragments_expected; 

  //number of fragments received
  uint16_t nfragments_rcvd;

  //number of bytes expected
  uint32_t nbytes_expected; 

  //bitmap of received fragments, up to maximum number of fragments
  //this is not in a standard order due to the way the way fragments are split amongst threads to avoid false sharing
  volatile uint64_t fragment_map[16]; 

  //flexible array member! 
  uint8_t bytes[];
}; 

struct turf_tx_ctx 
{
  int sock; 
  struct sockaddr addr; 
}

struct pueo_daq
{
  pueo_daq_config_t cfg; //The configuration, copied in


  //networking stuff
  struct 
  {
    struct in_addr turf_ip;
    struct in_addr turf_mask;

    int turf_ev_sck[DAQ_MAX_READER_THREADS]; 

    int turf_wr_sck;
    int turf_rd_sck;
    int turf_ctl_sck; 
    int turf_ack_sck;
    int turf_nck_sck;
  } net; 


  int evbuf_sz; // padded to nearest page... so not the same as cfg.max_ev_size 
  uint8_t * evmem; 
}; 

static inline struct event_buf * event_buf_get(pueo_daq_t * daq, int i)
{
  return (struct event_buf*)  (daq->evmem + i * daq->evbuf_sz); 
}



int event_buf_reset(struct event_buf * evbuf) 
{
  evbuf->nfragments_expected = 0;
  evbuf->nfragments_rcved = 0;
  evbuf->nbytes_expected = 0;
  evbuf->fragment_map = {0};
  // we can leave bytes alone... 
}


int pueo_daq_config_validate(const pueo_daq_config_t *cfg, FILE * out) 
{

  // Verify the network configuration is sane... 
  // Open a netlink socket
  int sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE); 
  struct ifreq ifr; 
  strncpy(ifr.ifr_name, cfg->eth, IFNAMSIZ); 
  int ret = 0; 

  //check the MTU 
  if (ioctl(sock, SIOCGIFMTU, &ifr) )
  {
    if (out) fprintf(out,"Could not query MTU (is %s a real device?)!!!\n", cfg->eth); 
    ret = -1; 
    goto end: 

  }
  if (ifr.ifr_mtu < cfg->fragment_size + 8) 
  {
    if (out) 
    {
      fprintf(out, "MTU of %d too small for fragment size of %d, trying to increase\n", ifr.ifr_mtu, fragment_size); 
    }

    //I suppose we could try to change it? 
    ifr.ifr_mtu = cfg->fragment_size+8; 
    if (ioctl(sock, SIOCSIFMTU, &ifr))
    {
      if (out) fprintf(out, "Could not set MTU to %d", ifr.ifr_mtu); 
      ret = -1; 
      goto end; 
    }
  }

  // TODO check that the device has an ip on the same subnet as the turf? 
  
  // TODO check route? 



end: 
  close(sock); 
  return ret 
}

static inline int next_page(int len) 
{
  const pgsize = 4096; 
  return ((pgsize-1) & len) ? ((len+pgsize) & ~(pgsize-1)) : len; 
}


static pueo_daq_config_t default_config = PUEO_DAQ_CONFIG_DFLT; 


pueo_daq_t * pueo_daq_init(const pueo_daq_config_t * cfg = NULL) 
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
  memcpy(daq->cfg, cfg, sizeof(pueo_daq_cfg)); 

  //set up buffers 
  daq->evbuf_sz = next_page(cfg.max_ev_size + sizeof(struct event_buf)); //round to page size 

  daq->evmem = calloc(daq->cfg.n_event_bufs, daq->evbuf_sz); 
  if (!daq->evmem) 
  {
    fprintf(stderr, "Could not allocate memory for event buffers!!!\n"); 
    goto bail; 
  }

  //set addresses 
  for (int i = 0; i < daq->evbuf_N; i++) 
  {
    event_buf_get(daq, i)->addr = i; 
  }
                                                                        
  // set turf address/mask 

  if (!inet_aton(cfg->turf_ip_addr, &daq->net.turf_ip))
  {
    fprintf(stderr, "Could not interpret %s as an IPV4 address\n", cfg->turf_ip_addr);
    goto bail; 
  }

  if (!inet_aton(cfg->turf_subnet_mask, &daq->net.turf_mask))
    fprintf(stderr, "Could not interpret %s as an IPV4 mask\n", cfg->turf_subnet_mask);
    goto bail; 
  }
  

  // open UDP sockets 

  daq->net.turf_wr_sck = socket(AF_INET, SOCK_DGRAM, 0); 
  daq->net.turf_rd_sck = socket(AF_INET, SOCK_DGRAM, 0); 
  daq->net.turf_ctl_sck = socket(AF_INET, SOCK_DGRAM, 0); 
  daq->net.turf_ack_sck = socket(AF_INET, SOCK_DGRAM, 0); 
  daq->net.turf_nck_sck = socket(AF_INET, SOCK_DGRAM, 0); 
  daq->net.turf_ev_sck = socket(AF_INET, SOCK_DGRAM, 0); 

  //verify that they all opened... 
 
  if (daq->net.turf_wr_sck < 0 || daq->net_turf.rd_sck < 0  || daq->net.turf_ctl_sck < 0 
      || daq->net.turf_ack_sck  < 0|| daq->net.turf.nck_sck < 0 || daq->net.turf_ev_sck < 0 )
  {
    fprintf(stderr,"Couldn't open sockets..."); 
    goto bail; 
  }

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
  close(daq->net.turf_wr_sck); 
  close(daq->net.turf_rd_sck); 
  close(daq->net.turf_ctl_sck); 
  close(daq->net.turf_ack_sck); 
  close(daq->net.turf_nck_sck); 
  close(daq->net.turf_ev_sck); 
  
  //deallocate buffers
  free(daq->evmem); 

  //deallocate the daq 
  free(daq); 
}



