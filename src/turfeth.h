#ifndef TURFETH_H
#define TURFETH_H

// Matches ICD v0.2+.

// This document details the structures specified in the TURF to SFC
// ICD. These use bitfield unions.
// Note: these assume struct bitfields get merged LSB to MSB.
// There is no way to force this but if GCC ever changes it
// pretty much everyone will raise holy hell.

// note: if this bothers you from a preference standpoint,
// just use this document as a reference and treat it as a weird
// way to write tables of bit fields.

#include <stdint.h>

typedef union {
  struct {
    uint32_t ADDR:28;  // address to read from
    uint32_t TAG:4;    // identifier for this request
  } BIT;
  uint32_t RAW;
} turf_rdreq_t;

typedef union {
  struct {
    uint64_t ADDR:28;     // address being acknowledged
    uint64_t TAG:4;       // request tag this is acknowledging
    uint64_t RDDATA:32;   // read data at this address
  } BIT;
  uint64_t RAW;
} turf_rdresp_t;

typedef union {
  struct {
    uint64_t ADDR:28;     // address to write to 
    uint64_t TAG:4;       // identifier for this request
    uint64_t WRDATA:32;   // data to write
  } BIT;
  uint64_t RAW;
} turf_wrreq_t;

typedef union {
  struct {
    uint64_t ADDR:28;   // address being acknowledged
    uint64_t TAG:4;     // request tag this is acknowledging
    uint64_t RSVD0:32;  // unused
  } BIT;
  uint64_t RAW;
} turf_wrresp_t;

typedef union {
  struct {
    uint64_t TOTAL:20;     // total length of this event (in bytes)
    uint64_t ADDR:12;      // address of this event
    uint64_t FRAGMENT:10;  // fragment number for this fragment
    uint64_t KID:22;       // constant
  } BIT;
  uint64_t RAW;
} turf_fraghdr_t;

typedef union {
  struct {
    uint64_t RSVD1:20; // unused
    uint64_t ADDR:12;  // address that was properly received
    uint64_t TAG:8;    // identifier for this ack
    uint64_t RSVD0:22; // unused
    uint64_t OPEN:1;   // must be set in a *confirmation* back, otherwise,
                       // the interface wasn't opened first
    uint64_t ALLOW:1;  // if 1, a new event is allowed out
  } BIT;
  uint64_t RAW;
} turf_ack_t;

typedef union {
  struct {
    uint64_t TOTAL:20; // length that was not properly received
                       // (send max if nothing received, I guess)
    uint64_t ADDR:12;  // address that was not properly received
    uint64_t TAG:8;    // identifier for this nack
    uint64_t OPEN:1;   // must be set in a *confirmation* back, otherwise
                       // the interface wasn't opened first
    uint64_t RSVD:1;   // unused
  } BIT;
  uint64_t RAW;
} turf_nack_t;

#define TURF_OP_COMMAND 0x4F50
#define TURF_CL_COMMAND 0x434C
#define TURF_ID_COMMAND 0x4944
#define TURF_PR_COMMAND 0x5052
#define TURF_PW_COMMAND 0x5057

typedef union {
  struct {
    uint64_t COMMAND:16; // 2 ASCII byte command (see above definitions)
    uint64_t PAYLOAD:48; // payload for command
  } BIT;
  uint64_t RAW;
} turf_ctl_t;

// This structure describes the command to or response from the
// PR or PW command. The PR command returns the maximum values of each
// of these. The PW command sets the current value.
// There's no way to read the current value because there's no point -
// just set it to what you want.
// The PW command has NO EFFECT is the command path is not open.
typedef union {
  struct {
    uint16_t FRAGSRCMASK:16; // fragment src mask (max or current)
    uint16_t ADDR:16;        // max addr for ack/nack
    uint16_t FRAGMENT:16;    // fragment payload (max or current) minus 1
  } BIT;
  uint64_t RAW;
} turf_ctl_param_t;

#endif 
