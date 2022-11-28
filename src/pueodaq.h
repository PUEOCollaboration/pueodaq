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
* Reading events can either be blocking (typically for debugging) or asynchronous. 
* Beware that a configurable number of threads will be spawned for reading in the non-blocking case.  
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
  /** Fragment size used for reading, MTU must be a bit bigger */ 
  uint16_t fragment_size; 

  /** Number of simultaneous events possible
   * in flight*/ 
  uint16_t n_event_bufs; 

  /** Maximum event size */ 
  uint32_t  max_ev_size; 

  /** Number of receiver threads, use 0 to use some default. 
   *  Maximum is probably 16. 
   **/
  uint8_t n_recvthreads; 

  /** TURF IP Address */
  const char * turf_ip_addr; 

  /** TURF Subnet Mask */ 
  const char * turf_subnet_mask; 

  /** Ethernet device, NULL to try to infer from TURF IP Address (is this actually needed, or do we just let the kernel route?*/
  const char * eth_device; 

} pueo_daq_config_t; 


// default configuration for pueo_daq_config, can do e.g. 
// pueo_daq_config_t cfg = PUEO_DAQ_CONFIG_DFLT;  
#define PUEO_DAQ_CONFIG_DFLT \
{\
  .fragment_size = 8192, \
  .n_event_bufs = 64, \
  .max_ev_size = 64, \
  .n_recvthreads = 4, \
  .turf_ip_addr = "192.168.1.128",\
  .turf_subnet_mask ="255.255.255.0",\
  .eth_device = NULL\
};


/* Opaque handle to DAQ*/ 
typedef struct pueo_daq pueo_daq_t; 

/** Validate the configuration */ 
int pueo_daq_config_validate(const pueo_daq_config_t * cfg, FILE* outbuf); 

/** Initialize a pueo_daq_t instance, default configuration used if cfg  is NULL*/
pueo_daq_t * pueo_daq_init(const pueo_daq_config_t * cfg); 

#define pueo_daq_default_init() pueo_daq_init(NULL) 

void pueo_daq_destroy(pueo_daq_t* daq); 

/** Dump debug info to stream */ 
int pueo_daq_dump(pueo_daq_t * daq, FILE * stream, int flags); 



/** Returns the number of complete events ready */ 
int pueo_daq_nready(pueo_daq_t * t) ; 






