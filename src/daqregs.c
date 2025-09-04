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


uint64_t read_dna(pueo_daq_t * daq, uint32_t device, const reg_t * reg)
{
  uint64_t dna = 0;
  if (write_based_reg(daq, device, reg, 0x80000000))
  {
    return (uint64_t) (-1);
  }

  for (int i = 0; i < 57; i++)
  {
    uint32_t val = 0;
    if (read_based_reg(daq,device,reg, &val))
    {
      return (uint64_t) (-1);
    }
    dna  = (dna << 1 ) & (val &1);
  }

  return dna;
}

