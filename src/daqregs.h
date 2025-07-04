#ifndef _DAQ_REGS_H
#define _DAQ_REGS_H

#include <stdint.h>
#include <stdbool.h>


typedef struct reg
{
  uint32_t addr;  //address of this register
  uint32_t reladdr;
  uint8_t offs;
  uint8_t len;
  uint32_t mask;
  bool ro;
  const char * name;
} reg_t;

enum
{
 RUNCMD_RESET =  2,
 RUNCMD_STOP =  3
} e_turf_constants;



//Helpers
//TODO: moar macro haxx to avoid passing around BASE so much
//it would be easy to do if I didn't want it done at compile time. j

#define MAKE_REG(BASE,REL,OFF,LEN,RO) .addr= BASE+REL , .reladdr = REL, .offs = OFF, .len = LEN, . mask = ((1ull << LEN)-1)<< OFF, .ro = RO 
#define REG_RO(BASE,reladdr) MAKE_REG(BASE, reladdr, 0, 32, true)
#define REG(BASE,reladdr) MAKE_REG(BASE, reladdr, 0, 32, false)
#define BF(BASE, reladdr, off, len) MAKE_REG(BASE,reladdr,off,len,false)
#define BF_RO(BASE, reladdr, off, len) MAKE_REG(BASE,reladdr,off,len,true)

#define DEF_DECLARE(NAME, DEF) reg_t NAME;
#define DEF_DEFINE(NAME, DEF) .NAME = { DEF, .name = #NAME },


#define REG_GROUP(NAME, BASE, REGS)\
static const struct {\
    uint32_t base; \
    const char *name;\
    REGS(DEF_DECLARE,BASE)\
  } NAME = { .base = BASE, .name = #NAME, REGS(DEF_DEFINE, BASE)  }



#define TURF_REGS(DEF, BASE) \
DEF(turfid,         REG_RO(BASE,0))\
DEF(dateversion,    REG_RO(BASE,4))\
DEF(dna,            REG_RO(BASE,8))\
DEF(sysclkmon,      REG_RO(BASE, 0x800))\
DEF(gbeclkmon,      REG_RO(BASE, 0x804))\
DEF(ddr0clkmon,     REG_RO(BASE, 0x808))\
DEF(ddr1clkmon,     REG_RO(BASE, 0x80c))\
DEF(aurcklmon,      REG_RO(BASE, 0x810))\
DEF(grxclkmon,      REG_RO(BASE, 0x814))\
DEF(gtxclkmon,      REG_RO(BASE, 0x818))\
DEF(bridgectrl,     REG(BASE, 0x1000))\
DEF(bridgestat,     REG(BASE, 0x1004))

REG_GROUP(turf, 0, TURF_REGS);

#define EVENT_REGS(DEF, BASE)\
DEF(event_reset,    BF(BASE,  0x0, 0, 1))\
DEF(event_in_reset, BF_RO(BASE,  0x0, 0, 1))\
DEF(mask,           BF(BASE,  0x0, 8, 4))\
DEF(ndwords0,       REG_RO(BASE, 0x010))\
DEF(ndwords1,       REG_RO(BASE, 0x014))\
DEF(ndwords2,       REG_RO(BASE, 0x018))\
DEF(ndwords3,       REG_RO(BASE, 0x01C))\
DEF(outqwords,      REG_RO(BASE, 0x020))\
DEF(outevents,      REG_RO(BASE, 0x024))\
DEF(count_reg,       REG_RO(BASE, 0x028))\
DEF(ack_count,       BF(BASE, 0x028, 0, 12))\
DEF(allow_count,     BF(BASE, 0x028, 16, 9))\
DEF(full_error,      REG_RO(BASE, 0x2c))


REG_GROUP(turf_event, 0x18000, EVENT_REGS);

typedef union
{
    struct
    {
      uint16_t ack_count : 12; //use uint16_t so it's alligned on the 16 bits
      uint16_t allow_count : 9;
    } as_count;
    uint32_t as_uint;
} event_count_reg_t;



#define TRIG_REGS(DEF,BASE)\
DEF(runcmd,               REG(BASE, 0x0))   \
DEF(fwudata,              REG(BASE, 0x4))   \
DEF(cratepps_en,          BF (BASE, 0x8, 0, 1))  \
DEF(mask,                 REG(BASE,0x100))  \
DEF(latency,              BF (BASE,0x104,0,16))  \
DEF(offset,               BF (BASE,0x104,0,16))  \
DEF(pps_reg,              REG(BASE,0x108))\
DEF(pps_trig_enable,      BF (BASE,0x108,0,1))   \
DEF(pps_offset,           BF (BASE,0x108,16,16)) \
DEF(ext_trig_enable,      BF (BASE,0x10c,0,1))   \
DEF(ext_trig_select,      BF (BASE,0x10c,1,1))   \
DEF(ext_offset,           BF (BASE,0x10c,16,16))   \
DEF(softrig,              REG(BASE, 0x110)) \
DEF(running,              BF (BASE, 0x110, 16,1)) \
DEF(occupancy,         REG_RO(BASE, 0x114)) \
DEF(holdoff_reg,          REG(BASE, 0x118)) \
DEF(holdoff,              BF (BASE, 0x118, 0, 16)) \
DEF(surf_err,             BF (BASE, 0x118, 16, 1)) \
DEF(turf_err,             BF (BASE, 0x118, 17, 1)) \
DEF(trigger_count,     REG_RO(BASE, 0x11c))\
DEF(scaler_base,       REG_RO(BASE, 0x300))\
DEF(scaler_max,       REG_RO(BASE, 0x37c))

REG_GROUP(turf_trig, 0x1c000, TRIG_REGS);

//TODO  this should be automatically generated probably
typedef union
{
    struct
    {
      uint32_t holdoff : 16;
      uint32_t surf_err : 1;
      uint32_t turf_err : 1;
    } as_holdoff;
    uint32_t as_uint;
} holdoff_reg_t;


#define TIME_REGS(DEF,BASE)            \
DEF(en_int_pps,     BF    (BASE,0,0,1))    \
DEF(use_int_pps,    BF    (BASE,0,1,1))    \
DEF(pps_holdoff,    BF    (BASE,0,16,16))  \
DEF(current_second, REG   (BASE,0x08))    \
DEF(last_pps,       REG_RO(BASE,0x0c))    \
DEF(llast_pps,      REG_RO(BASE,0x010))\
DEF(last_dead,      REG_RO(BASE,0x014)) \
DEF(llast_dead,     REG_RO(BASE,0x018)) \
DEF(panic_counter,  REG_RO(BASE,0x01c))


REG_GROUP(turf_time, 0x1a000, TIME_REGS);


//undef some names that might clash
#undef REG
#undef REG_RO
#undef BF
#endif
