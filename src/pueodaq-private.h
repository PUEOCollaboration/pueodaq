#ifndef _PUEO_DAQ_PRIVATE_H
#define _PUEO_DAQ_PRIVATE_H

#include <stdatomic.h>
#include "turfeth.h"

typedef union fpga_id
{
  char c[4];
  uint32_t u;
} fpga_id_t;

#define FPGAID(x) (fpga_id_t) { .u = ntohl(x) }

#define DELTA_T(now, then)  ( ( now.tv_sec - then.tv_sec) + 1e-9 * (now.tv_nsec - then.tv_nsec))




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

struct reader_thread_setup
{
  uint16_t tnum;
  pueo_daq_t * daq;
};




typedef union datever
{
  struct
  {
    uint8_t rev;
    uint8_t minor : 4;
    uint8_t major : 4;
    uint16_t day : 5;
    uint16_t month : 4;
    uint16_t year : 7;
  } decoded;
  uint32_t as_uint;
} datever_t;


//event buffer implementation
struct event_buf
{

  volatile _Atomic(uint64_t) first_fragment_packed_time;
  volatile _Atomic(uint64_t) last_fragment_packed_time;

  uint64_t event_index; // Sequential event number received
                         //
  uint32_t run_number; // Set from daq config
                         //
  //number of bytes expected
  uint32_t nbytes_expected;

  uint32_t header_size;
  uint32_t nsamples;

  //address for this buf (on the TURF)
  volatile uint16_t address;

  //number of fragment expected
  volatile uint16_t  nfragments_expected;

  //number of fragments received
  volatile _Atomic(uint16_t) nfragments_rcvd;



  //list of fragment indices, this will be ceil(max_event_size / fragment_size) big
  uint32_t fragments[];
};



#define DFLT_RECV_THREADS 1
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

    _Atomic uint32_t wr_tag; //  &0xf

    _Atomic uint32_t rd_tag;  // & 0xf


    // these are receiving ports on
    int daq_ctl_sck;
    int daq_frgctl_sck;
    int daq_frg_sck[DAQ_MAX_READER_THREADS];

  } net;

  struct
  {
    uint32_t turfid;
    datever_t turf_datever;
    uint64_t turf_dna;
    struct
    {


      uint32_t turfioid;
      uint64_t turfio_dna;
      datever_t turfio_datever;
      uint32_t surfid[NSURFSLOTS];
      uint64_t surf_dna[NSURFSLOTS];
      datever_t surf_datever[NSURFSLOTS];
    } turfio[NTFIO];
  } census;


  // the event buffer
  uint32_t evbuf_sz;
  struct event_buf * event_bufs;
  uint32_t evbuf_bitmap_size;
  volatile atomic_uint_fast16_t last_free_evbuf;
  volatile atomic_uint_fast16_t last_sent_evbuf;
  volatile atomic_uint_fast64_t * event_bufs_inuse_bitmap; //marked if started receiving
  volatile atomic_uint_fast64_t * event_bufs_ready_bitmap; //marked if ready to readout


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
  struct reader_thread_setup reader_thread_setups[DAQ_MAX_READER_THREADS];

  // The control thread (acks and nacks and address assignment)
  pthread_t ctl_thread;

  /*
  * This gives us the address map to event number.
  *
  * This gets updated at the beginning and we ack an event by the ctl thread
  */
  volatile atomic_int_fast16_t  addr_map[TURF_NUM_ADDR];

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

  pueo_L2_stat_t cached_l2;
  pueo_L1_stat_t cached_l1[4][7];
  pueo_daq_scalers_t cached_scalers;

};


#endif
