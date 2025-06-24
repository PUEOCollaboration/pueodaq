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

  struct
  {
    uint16_t control_in;
    uint16_t fragcontrol_in;
    uint16_t fragment_in;
  } daq_ports;  ///

  struct timespec timeout;
  size_t max_attempts;
  int debug;
  uint32_t turfio_mask : 4;


} pueo_daq_config_t;
/** Opaque handle to DAQ*/
typedef struct pueo_daq pueo_daq_t;


typedef struct pueo_daq_event_data
{
  union
  {
   struct
   {
     uint16_t hdr_size_min_div4_m1;
     uint16_t hdr_version;
   } tag;
   struct
   {
     uint8_t v[PUEODAQ_MAX_HEADER_SIZE];
   } bytes;
  } header;
  int16_t waveform_data[PUEODAQ_NCHAN][PUEODAQ_NSAMP]; //TODO parameterize this, make it flexible maybe. This used to be flexible but right now the firmware isn't flexible so...
} pueo_daq_event_data_t;


typedef int (*pueo_daq_event_ready_callback_t)(pueo_daq_t * daq, uint32_t idx);

/**  default configuration for pueo_daq_config, can do e.g.
/ pueo_daq_config_t cfg =  { PUEO_DAQ_CONFIG_DFLT };
/ you can override anything afterwards if you want to, like
/ pueo_daq_config_t cfg =  { PUEO_DAQ_CONFIG_DFLT, .fragment_size = 5000};
*/
#define PUEO_DAQ_CONFIG_DFLT                \
  .fragment_size = 8184,                    \
  .n_event_bufs = 512,             \
  .max_in_flight = 32,                      \
  .max_ev_size = 1 << 20,                   \
  .n_recvthreads = 4,                       \
  .turf_ip_addr = "10.68.65.81",            \
  .turf_subnet_len = 24,                    \
  .eth_device = NULL,                       \
  .daq_ports = {                            \
    .control_in = 0x5263,                   \
    .fragcontrol_in =0x5266,                \
    .fragment_in = 0x5278,                  \
  },                                        \
  .timeout = {.tv_sec = 0, .tv_nsec = 1e7 }, \
  .max_attempts = 10, .debug = false, .turfio_mask = 0x0



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


typedef struct pueo_daq_stats
{
  uint32_t turfio_words_recv[4];
  uint32_t qwords_sent;
  uint32_t events_sent;
} pueo_daq_stats_t;

#define PUEODAQ_STATS_JSON_FORMAT_WITH_PREFIX(prefix)  prefix "\"turfio0_recv_bytes\": %u,\n"\
                                                       prefix "\"turfio1_recv_bytes\": %u,\n"\
                                                       prefix "\"turfio2_recv_bytes\": %u,\n"\
                                                       prefix "\"turfio3_recv_bytes\": %u,\n"\
                                                       prefix "\"bytes_sent\": %u,\n"\
                                                       prefix "\"events_sent\": %u,\n"

#define PUEODAQ_STATS_JSON_FORMAT PUEODAQ_STATS_JSON_FORMAT_WITH_PREFIX("  ")

#define PUEODAQ_STATS_VALS(s)  4*s.turfio_words_recv[0], 4*s.turfio_words_recv[1], 4*s.turfio_words_recv[2], 4*s.turfio_words_recv[3], 8*s.qwords_sent, s.events_sent


int pueo_daq_get_stats(pueo_daq_t * daq,  pueo_daq_stats_t * stats);





