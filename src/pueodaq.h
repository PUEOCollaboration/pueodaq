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
  uint16_t fragment_size; /// Fragment size used for reading, MTU must be a bit bigger  

  uint16_t n_event_bufs; /// Number of simultaneous events possible in flight

  uint32_t  max_ev_size; /// Maximum event size  

  uint8_t n_recvthreads; /// Number of receiver threads, use 0 to use some default. Maximum is probably 16. 

  const char * turf_ip_addr;   /// TURF IP Address 

  const char * turf_subnet_mask; /// TURF Subnet Mask

  const char * eth_device; /// Ethernet device, NULL to try to infer from TURF IP Address

} pueo_daq_config_t; 


/**  default configuration for pueo_daq_config, can do e.g. 
/ pueo_daq_config_t cfg =  { PUEO_DAQ_CONFIG_DFLT }; 
/ you can override anything afterwards if you want to, like 
/ pueo_daq_config_t cfg =  { PUEO_DAQ_CONFIG_DFLT, .fragment_size = 16384}; 
*/ 
#define PUEO_DAQ_CONFIG_DFLT \
  .fragment_size = 8192, \
  .n_event_bufs = 64, \
  .max_ev_size = 64, \
  .n_recvthreads = 4, \
  .turf_ip_addr = "192.168.1.128",\
  .turf_subnet_mask ="255.255.255.0",\
  .eth_device = NULL\


/** Opaque handle to DAQ*/ 
typedef struct pueo_daq pueo_daq_t; 

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
int pueo_daq_nready(pueo_daq_t * t) ; 






