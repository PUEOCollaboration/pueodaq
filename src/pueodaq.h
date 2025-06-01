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
* Events are stored in a ring buffer. This may be allocated internally or externally provided.
* To use internal storage, configure n_event_bufs_to_alloc to non-zero number and don't call an internal buffer.
*
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


#include <stdint.h>
#include <stdio.h>

/**
 * Configuration for pueo_daq
 *
 **/
typedef struct pueo_daq_config
{
  uint16_t fragment_size; /// Fragment size used for reading, MTU must be a bit bigger

  uint16_t n_event_bufs_to_alloc; /// Number of event buffers to allocate. Set to 0 if using external buffer!

  uint16_t max_in_flight ; /// Maximum number of events in flight

  uint32_t  max_ev_size; /// Maximum event size (in fragments)

  uint8_t n_recvthreads; /// Number of receiver threads, use 0 to use some default. Maximum is probably 16.

  const char * turf_ip_addr;   /// TURF IP Address

  uint8_t turf_subnet_len; /// TURF subnet prefix length (the thing after the slash in CIDR notation)

  const char * eth_device; /// Ethernet device, NULL to try to infer via route from TURF IP Address

  struct
  {
    uint16_t control_in;
    uint16_t fragcontrol_in;
    uint16_t fragment_in;
    uint16_t tx_out;
  } daq_ports;  ///

  struct timespec timeout; //timeout for read/write operations
  size_t max_attempts;


} pueo_daq_config_t;
/** Opaque handle to DAQ*/
typedef struct pueo_daq pueo_daq_t;


typedef struct pueo_daq_event_data
{
  uint16_t addr;
  uint8_t thd;
  uint8_t reserved;
  uint32_t nbytes;
  const uint8_t * data;
} pueo_daq_event_data_t;

typedef int (*pueo_daq_event_ready_callback_t)(pueo_daq_t * daq, const pueo_daq_event_data_t * event_data);

/**  default configuration for pueo_daq_config, can do e.g.
/ pueo_daq_config_t cfg =  { PUEO_DAQ_CONFIG_DFLT };
/ you can override anything afterwards if you want to, like
/ pueo_daq_config_t cfg =  { PUEO_DAQ_CONFIG_DFLT, .fragment_size = 16384};
*/
#define PUEO_DAQ_CONFIG_DFLT                \
  .fragment_size = 8192,                    \
  .n_event_bufs_to_alloc = 64,              \
  .max_ev_size = 64,                        \
  .n_recvthreads = 4,                       \
  .turf_ip_addr = "10.68.65.81",            \
  .turf_subnet_len = 24,                    \
  .eth_device = NULL,                       \
  .daq_ports = {                            \
    .control_in = 0x5263,                   \
    .fragcontrol_in =0x5266,                \
    .fragment_in = 0x5278,                  \
    .tx_out = 0x5478                        \
  },                                        \
  .timeout = {.tv_sec = 0, .tv_nsec = 10000000 }, \
  .max_attempts = 10



/** Validate the configuration
 * @param cfg the configuration to validate
 * @param outbuf where to write output to (e.g. stdout, stderr)
 * @returns 0 on success
 * */
int pueo_daq_config_validate(const pueo_daq_config_t * cfg, FILE* outbuf);

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


/** Returns the size in bytes required
 *  to store a single event given current configuration. Useful if using an external buffer and you
 *  want to fit a certain number of events
 *
 *  @param daq the DAQ handle
 *  */

size_t pueo_daq_get_event_size(const pueo_daq_t * daq);


/**
 * Use this to set a single event buffer.
 *
 * This will override any previous buffers. If pueo_daq_t had already allocated buffers
 * because it was told to in its config, those will be freed, but any externally
 * allocated buffers will be left alone.
 *
 * */
int pueo_daq_set_buffer(pueo_daq_t * daq, size_t nbytes, void * buf);



/** Copies out an event from the buffer, then releaes the DAQ buffer.
 * For performance-sensitive cases you'd typically not use this!
 * */
int pueo_daq_copy_and_release_event(pueo_daq_t * daq,  pueo_daq_event_data_t * dest);


/** Borrow an event from the buffer.
 * It will not be freed until you free its addesss with pueo_daq_release_event.
 * */
int pueo_daq_borrow_event(pueo_daq_t *daq, const pueo_daq_event_data_t ** refdest);


/** Release an event from a buffer. It will eventually be overwritten with a new event.
 */
int pueo_daq_release_event(pueo_daq_t *daq, uint16_t address);

/** Register a callback that is called when an event is ready
 * for asynchronous event retrieval.
 */
int pueo_daq_register_ready_callback(pueo_daq_t * daq, pueo_daq_event_ready_callback_t cb);


int pueo_daq_set_run_number(pueo_daq_t * daq, uint32_t run);

// Start taking data!
int pueo_daq_start(pueo_daq_t * daq);

// Stop taking data
int pueo_daq_stop(pueo_daq_t * daq);


// synchronous raw read/write to DAQ. These will block until we get the response.
// Right now these are also locking but that will probably change
int pueo_daq_write(pueo_daq_t *daq, uint32_t wraddr, uint32_t data);
int pueo_daq_read(pueo_daq_t *daq, uint32_t rdaddr, uint32_t * data);


