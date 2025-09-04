#include "pueodaq.h"
#include "daqregs.h"
#include <assert.h>

int read_based_reg(pueo_daq_t * daq, uint32_t base, const reg_t * reg, uint32_t * val)
{

  int r = pueo_daq_read(daq, reg->addr+base, val);
  (*val) &=reg->mask;
  (*val)>>=reg->offs;
  return r;
}


int write_based_reg(pueo_daq_t * daq, uint32_t base, const reg_t * reg, uint32_t val)
{
  assert (!reg->ro);
  val <<= reg->offs;
  val &= reg->mask;

  // we have to load the old value in this case
  if (reg->len!=32)
  {
    uint32_t current = 0;
    if (read_based_reg(daq, base, reg, &current))
    {
      fprintf(stderr,"Could not read reg for partial write\n");
      return -1;
    }
    val |= current & (~reg->mask);
  }

  return pueo_daq_write(daq, base+reg->addr, val);
}

