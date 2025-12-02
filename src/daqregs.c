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



int write_incrementing_regs(pueo_daq_t * daq, uint8_t N, uint32_t base, const reg_t * reg, const uint32_t * val)
{
  pueo_daq_many_setup_t s = { .N = N, .addr_offset = base, .addr_start = reg->addr, .addr_step = 4, .wr_data_v = val };
  return pueo_daq_write_many(daq, &s);
}

int read_incrementing_regs(pueo_daq_t * daq, uint8_t N, uint32_t base, const reg_t * reg, uint32_t * val)
{

  pueo_daq_many_setup_t s = { .N = N, .addr_offset = base, .addr_start = reg->addr, .addr_step = 4, .rd_data_v = val };


  int r =   pueo_daq_read_many(daq, &s);
  for (int i = 0; i < N; i++)
  {
    val[i] &=  reg->mask;
    val[i] >>= reg->offs;
  }
  return r;

}

int read_based_regs(pueo_daq_t * daq, uint8_t N, uint32_t base, const reg_t ** reg, uint32_t * val)
{

  static __thread uint32_t addrs[PUEODAQ_MAX_MANY_SIZE];
  for (int i = 0; i < N; i++)
  {
    addrs[i] = base + reg[i]->addr;
  }
  pueo_daq_many_setup_t s = { .N = N, .addr_v = addrs, .rd_data_v = val };


  int r = pueo_daq_read_many(daq, &s);

  for (int i = 0; i < N; i++)
  {
    val[i] &=  reg[i]->mask;
    val[i] >>= reg[i]->offs;
  }

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
    reg_t readback =  { .addr = reg->addr, .len = 32, .reladdr = reg->reladdr, .mask = 0xffffffff };

    if (read_based_reg(daq, base, &readback, &current))
    {
      fprintf(stderr,"Could not read reg for partial write\n");
      return -1;
    }

//    if (daq->cfg.debug > 1)  ("write_reg with readback, base=0x%x, addr = 0x%x, current=0x%x, val_before=0x%x\n", base, reg->addr, current, val);
    val |= current & (~reg->mask);
//    if (daq->cfg.debug > 1) printf("val after: 0x%x\n", val);
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

