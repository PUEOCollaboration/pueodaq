/*
*
* pueodaq.h
*
* This file is part of pueodaq,
*
* This is the main DAQ API.
*
* Initializing an instance will generate an opaque object, which handles all network connections.
*
* Configuring/writing to the TURF is synchronous.
*
* Reading events has both a blocking and callback API.
* Beware that a configurable number of threads will be spawned for reading, if configured.
*
* Events are stored in a ring buffer, allocated internally for now.
*
*
* Contributing Authors:  cozzyd@kicp.uchicago.edu
*
* Copyright (C) 2022-  PUEO Collaboration
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/


#define PUEODAQ_NCHAN 224
#define PUEODAQ_NSAMP 1024
#define PUEODAQ_MAX_HEADER_SIZE 1024

#define PUEODAQ_MAX_MANY_SIZE 256

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <netinet/in.h>

/**
 * Configuration for pueo_daq
 *
 **/
typedef struct pueo_daq_config
{
  uint16_t fragment_size; /// Fragment size used for reading, MTU must be a bit bigger by the header size (8 bytes)

  uint16_t n_event_bufs; /// Number of event buffers to allocate.

  uint8_t max_in_flight ; /// Maximum number of events in flight (must be less than n_event_bufs and no bigger than 256).

  uint32_t  max_ev_size; /// Maximum event size (in bytes) (max 1 <<20)

  uint8_t n_recvthreads; /// Number of receiver threads, use 0 to use some default. Maximum is probably 16.

  const char * turf_ip_addr;   /// TURF IP Address

  uint8_t turf_subnet_len; /// TURF subnet prefix length (the thing after the slash in CIDR notation)

  const char * eth_device; /// Ethernet device, NULL to try to infer via route from TURF IP Address

  uint8_t frag_src_mask;

  struct
  {
    uint16_t control_in;
    uint16_t fragcontrol_in;
    uint16_t fragment_in;
  } daq_ports;  ///

  struct timespec timeout;
  size_t max_attempts;
  int debug;

  //things that don't really change at runtime?
  struct
  {
    uint32_t turfio_mask : 4; //note that 0 means working here, opposite of how I would define it
    uint16_t offset;
    uint16_t latency;
    uint8_t  rundly;
  } trigger;


} pueo_daq_config_t;
/** Opaque handle to DAQ*/
typedef struct pueo_daq pueo_daq_t;

typedef union pueo_daq_event_header
{
   struct
   {
     uint16_t header_words_m1; // number of header words after this one
     uint16_t header_version;
     uint32_t event_number;
     uint32_t event_second;
     uint32_t event_time;
     uint32_t last_pps;
     uint32_t llast_pps;
     uint32_t trigger_meta[4];
     uint16_t reserved[34];
     uint16_t tfio_mask :4;
     uint16_t tid : 12;
     uint16_t surf_header_words;
     uint64_t transposed_surf_headers[4];
   } vals;

   struct
   {
     uint8_t v[PUEODAQ_MAX_HEADER_SIZE]; // reserve 1024 bytes (the maximum)
   } bytes;


} pueo_daq_event_header_t;


#define PUEO_DAQ_EVENT_HEADER_FORMAT\
  "{\n  \"header_words\": %u,\n  \"header_version\":  %hu,\n"\
  "  \"event_number\": %u,\n  \"event_second\":  %u,\n"\
  "  \"event_time\": %u,\n  \"last_pps\":  %u,\n"\
  "  \"llast_pps\": %u,\n  \"trigger_meta\":  [%u,%u,%u,%u],\n"\
  "  \"rfio_mask\": 0x%x,\n  \"tid\":  0x%x,\n"\
  "  \"surf_header_words\": %hu,\n  \"transposed_surf_headers\":  [0x%llx, 0x%llx, 0x%llx, 0x%llx]\n}\n"

#define PUEO_DAQ_EVENT_HEADER_VALUES(HDR)\
  HDR.vals.header_words_m1 + 1, HDR.vals.header_version, HDR.vals.event_number, HDR.vals.event_second, HDR.vals.event_time, HDR.vals.last_pps, HDR.vals.llast_pps,\
  HDR.vals.trigger_meta[0], HDR.vals.trigger_meta[1], HDR.vals.trigger_meta[2], HDR.vals.trigger_meta[3], HDR.vals.tfio_mask, HDR.vals.tid, HDR.vals.surf_header_words,\
  HDR.vals.transposed_surf_headers[0], HDR.vals.transposed_surf_headers[1], HDR.vals.transposed_surf_headers[2], HDR.vals.transposed_surf_headers[3]

typedef struct pueo_daq_event_data
{
  pueo_daq_event_header_t header;
  int16_t waveform_data[PUEODAQ_NCHAN][PUEODAQ_NSAMP]; //TODO parameterize this, make it flexible maybe. This used to be flexible but right now the firmware isn't flexible so...
} pueo_daq_event_data_t;


typedef int (*pueo_daq_event_ready_callback_t)(pueo_daq_t * daq, uint32_t idx);

/**  default configuration for pueo_daq_config, can do e.g.
/ pueo_daq_config_t cfg =  { PUEO_DAQ_CONFIG_DFLT };
/ you can override anything afterwards if you want to, like
/ pueo_daq_config_t cfg =  { PUEO_DAQ_CONFIG_DFLT, .fragment_size = 5000};
*/
#define PUEO_DAQ_CONFIG_DFLT                \
  .fragment_size = 8192,                    \
  .n_event_bufs = 512,                      \
  .max_in_flight = 32,                      \
  .max_ev_size = 1 << 20,                   \
  .n_recvthreads = 4,                       \
  .turf_ip_addr = "10.68.65.81",            \
  .turf_subnet_len = 24,                    \
  .frag_src_mask = 0x3f,                    \
  .eth_device = NULL,                       \
  .daq_ports = {                            \
    .control_in = 0x5263,                   \
    .fragcontrol_in =0x5266,                \
    .fragment_in = 0x5278,                  \
  },                                        \
  .timeout = {.tv_sec = 0, .tv_nsec = 1e8 }, \
  .max_attempts = 10, .debug = false,\
  .trigger = { .turfio_mask = 0x0, .offset = 39, .latency = 0, .rundly = 3 }



/** Validate the configuration
 * @param cfg the configuration to validate
 * @param outbuf where to write output to (e.g. stdout, stderr)
 * @param ourip On success, filled with a valid ip we have on the correct subnet/device as the TURF
 * @returns 0 on success
 * */
int pueo_daq_config_validate(const pueo_daq_config_t * cfg, FILE* outbuf, struct in_addr * our_ip);

/** Initialize a pueo_daq_t instance
 * @param cfg the configuration. The default is used if NULL.
 * @returns an opaque handle to the daq, or NULL if something failed.
 * */
pueo_daq_t * pueo_daq_init(const pueo_daq_config_t * cfg);


/** convenience wrapper  */
#define pueo_daq_default_init() pueo_daq_init(NULL)

/** Uninitialize the DAQ
 * @param daq the DAQ handle
 * */
void pueo_daq_destroy(pueo_daq_t* daq);

#define PUEODAQ_DUMP_INCLUDE_L1 1
/** Dump debug info to stream
 *
 * @param daq the DAQ handle
 * @param stream a FILE to write to (e.g. stdout)
 * @param flags TBD
 * @returns number of bytes written
 * */
int pueo_daq_dump(pueo_daq_t * daq, FILE * stream, int flags);



/** Returns the number of complete events ready
 * @param daq the DAQ handle
 * @returns number of complete events ready to be read
 * */
int pueo_daq_nready(const pueo_daq_t * daq) ;


int pueo_daq_soft_trig(pueo_daq_t * daq);

/** Copies out an event, then releases the DAQ buffer.
 * If none is ready, this will block. (You can pass NULL to just eat an event).
 * */
int pueo_daq_get_event(pueo_daq_t * daq,  pueo_daq_event_data_t * dest);

/** Blocks until an event is ready */
void pueo_daq_wait_event(pueo_daq_t * daq);



/** Register a callback that is called when an event is ready
 * for asynchronous event retrieval.
 */
void pueo_daq_register_ready_callback(pueo_daq_t * daq, pueo_daq_event_ready_callback_t cb);

void  pueo_daq_set_run_number(pueo_daq_t * daq, uint32_t run);

// Start taking data!
int pueo_daq_start(pueo_daq_t * daq);

// Stop taking data
int pueo_daq_stop(pueo_daq_t * daq);

//reset daq state
int pueo_daq_reset(pueo_daq_t * daq);

// synchronous raw read/write to DAQ. These will block until we get the response.
// Right now these are also locking but that will probably change
int pueo_daq_write(pueo_daq_t *daq, uint32_t wraddr, uint32_t data);
int pueo_daq_read(pueo_daq_t *daq, uint32_t rdaddr, uint32_t * data);

typedef struct pueo_daq_many_setup
{
  unsigned N; //number to read
  unsigned addr_stride; // 0 same as 1
  unsigned data_stride; // 0 same as 1
  const uint32_t * addr_v; //NULL ok to use
  uint32_t addr_offset; // added to each address, unconditionally
  uint32_t addr_start;  // used if addr_v is NULL to define a range
  uint32_t addr_step;  // used if addr_v is NULL
  const uint32_t * addr_offset_v;  // if not NULL wil lbe added individually to read addresses

  uint32_t * rd_data_v;  //used if reading
  const uint32_t * wr_data_v;  //used if writing
  uint32_t * const * rd_indirect_data_v;  //used if reading and rd_data_v is NULL
  const uint32_t * const * wr_indirect_data_v;  //used if writing and wr_data_v is NULL
  uint32_t wr_val; // used if wr_data_v is NULL and wr_indirect_data_v is NULL


} pueo_daq_many_setup_t;

int pueo_daq_read_many(pueo_daq_t *daq, const pueo_daq_many_setup_t * s);
int pueo_daq_write_many(pueo_daq_t *daq, const pueo_daq_many_setup_t * s);


typedef struct pueo_daq_stats
{
  uint32_t turfio_words_recv[4];
  uint32_t qwords_sent;
  uint32_t events_sent;
  uint32_t trigger_count;
  uint32_t current_second;
  uint32_t last_pps;
  uint32_t llast_pps;
  uint32_t last_dead;
  uint32_t llast_dead;
  uint32_t panic_count;
  uint32_t occupancy;
  uint16_t ack_count;
  uint32_t full_err[3];
  uint32_t completion_count;
  uint16_t allow_count;
  uint16_t holdoff;
  uint16_t pps_trig_offset;
  bool running;
  bool in_reset;
  bool surf_err;
  bool turf_err;
  bool pps_trig_enabled;
  bool leveltwo_logic;
  bool rf_trig_en;
  uint32_t trigger_mask;
} pueo_daq_stats_t;

#define PUEODAQ_STATS_JSON_FORMAT_WITH_PREFIX(prefix)  prefix "\"turfio0_recv_bytes\": %llu,\n"\
                                                       prefix "\"turfio1_recv_bytes\": %llu,\n"\
                                                       prefix "\"turfio2_recv_bytes\": %llu,\n"\
                                                       prefix "\"turfio3_recv_bytes\": %llu,\n"\
                                                       prefix "\"bytes_sent\": %llu,\n"\
                                                       prefix "\"events_sent\": %u,\n"\
                                                       prefix "\"trig_count\": %u,\n"\
                                                       prefix "\"current_sec\": %u,\n"\
                                                       prefix "\"pps\": %u, \"last_pps\": %u\n"\
                                                       prefix "\"pps_trig_offset\": %u,\n"\
                                                       prefix "\"dead\": %u, \"last_dead\": %u,\n"\
                                                       prefix "\"panic_count: \" %u\n"\
                                                       prefix "\"occupancy: \" %u\n"\
                                                       prefix "\"ack_count: \" %u\n"\
                                                       prefix "\"allow_count: \" %u\n"\
                                                       prefix "\"holdoff: \" %u\n"\
                                                       prefix "\"running\": %s, \"in_reset\": %s,\n"\
                                                       prefix "\"full_err:\": [0x%x,0x%x,0x%x],\n"\
                                                       prefix "\"turf_err\": %s, \"surf_err\": %s, \"pps_enabled\": %s,\n"\
                                                       prefix "\"leveltwo_logic\": %s, \"rf_trig_en\": %s,\n"\
                                                       prefix "\"trigger_mask\": 0x%x"

#define PUEODAQ_STATS_JSON_FORMAT PUEODAQ_STATS_JSON_FORMAT_WITH_PREFIX("  ")

#define PUEODAQ_STATS_VALS(s)  4ull*s.turfio_words_recv[0], 4ull*s.turfio_words_recv[1], 4ull*s.turfio_words_recv[2], 4ull*s.turfio_words_recv[3],\
       	8ull*s.qwords_sent, s.events_sent, s.trigger_count, s.current_second, s.last_pps, s.llast_pps, s.pps_trig_offset, s.last_dead, s.llast_dead, \
	s.panic_count, s.occupancy, s.ack_count, s.allow_count, s.holdoff, s.running ? "true" : "false" , s.in_reset ? "true" : "false", s.full_err[0], s.full_err[1], s.full_err[2],\
  s.turf_err ? "true" : "false", s.surf_err ? "true" : "false", s.pps_trig_enabled ? "true" : "false", s.leveltwo_logic ? "\"OR\"" : "\"AND\"", s.rf_trig_en  ? "true" : "false", s.trigger_mask


int pueo_daq_get_stats(pueo_daq_t * daq,  pueo_daq_stats_t * stats);



typedef struct pueo_daq_scalers
{
  struct timespec readout_time;
  union
  {
    uint32_t v[32]; // by scaler
    struct
    {
      uint32_t turfio0_slots[7];
      uint32_t soft;
      uint32_t turfio1_slots[7];
      uint32_t pps;
      uint32_t turfio2_slots[7];
      uint32_t ext;
      uint32_t turfio3_slots[7];
      uint32_t reserved;
    } map;
  }scalers;
} pueo_daq_scalers_t;


int pueo_daq_pps_setup(pueo_daq_t *daq, bool enable, uint16_t offset);
int pueo_daq_enable_rf_readout(pueo_daq_t * daq, bool enable);

int pueo_daq_get_scalers(pueo_daq_t * daq, pueo_daq_scalers_t* s);
int pueo_daq_scalers_dump(FILE *f, const pueo_daq_scalers_t * s);

int pueo_daq_set_L1_thresholds(pueo_daq_t * daq, int surf_link, int surf_slot, const uint32_t *thresholds, const uint32_t * pseudothresholds);

// using my convention here, where 1 means working, 0 means off
// NOTE THAT THIS AFFECTS THE SCALERSSSSSS SO PROBABLY DON'T USE THIS
int pueo_daq_set_L1_masks(pueo_daq_t * daq, int surf_link, int surf_slot, uint64_t beam_mask);

#define PUEO_L1_BEAMS 48
typedef struct pueo_L1_stat
{
  struct
  {
    uint64_t threshold : 18;
    uint64_t pseudothreshold : 18;
    uint64_t scaler : 12;
    uint64_t pseudoscaler : 12;
    uint64_t in_beam_mask : 1;
    uint64_t scaler_bank_before : 1;
    uint64_t scaler_bank_after : 1;
  } beams[PUEO_L1_BEAMS];

  struct timespec readout_time_start;
  uint16_t ms_elapsed;
  uint8_t surf_link : 2;
  uint8_t surf_slot : 3;
  uint8_t flags;
} pueo_L1_stat_t;

typedef struct pueo_L2_stat
{
  struct timespec readout_time_start;
  uint32_t Hscalers[12];
  uint32_t Vscalers[12];
} pueo_L2_stat_t;

int pueo_daq_L1_stat_dump(FILE *f, const pueo_L1_stat_t * s);
int pueo_daq_L2_stat_dump(FILE *f, const pueo_L2_stat_t * s);


int pueo_daq_read_L1_stat(pueo_daq_t * daq, int surf_link, int surf_slot, pueo_L1_stat_t * stat);
int pueo_daq_read_L2_stat(pueo_daq_t * daq, pueo_L2_stat_t * stat);

/** Here, unlike on the SURF, 1 means enabled, not disabled */
int pueo_daq_set_L2_mask(pueo_daq_t * daq, uint32_t mask);

// 0-12 are MI, 13 is LF , 1 means enabled (i.e. triggering), 0 means not
int pueo_daq_set_L2_mask_by_2phi(pueo_daq_t * daq, uint16_t H, uint16_t V);

