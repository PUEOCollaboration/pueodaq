#include "pueodaq.h"
#include "pueo-biquad-coeffs.h"
#include "daqregs.h"
#include "pueodaq-private.h"

#define BIQUAD_BASE(l, s, c, b)   ( SURF_BASE(l,s) + 0x80 * (b & 1) + 0x400 * (c & 0x7))

int pueo_daq_bypass_biquad(pueo_daq_t * daq, int surf_link, int surf_slot, int channel, int bq) 
{
  uint32_t base = BIQUAD_BASE(surf_link, surf_slot, channel, bq);

  return write_based_reg(daq,base, &surf_bq.update_control, 0x800000);
}

int pueo_daq_set_biquad(pueo_daq_t *daq, int surf_link, int surf_slot, int channel, int bq_idx, const pueo_biquad_t *bq)
{
  uint32_t base = BIQUAD_BASE(surf_link, surf_slot, channel, bq_idx);
  //printf("USING BASE %x for SURF L%d S%d CH%d BQ%d\n", base, surf_link, surf_slot, channel, bq_idx);
  //int old_debug = daq->cfg.debug;
  //daq->cfg.debug = 10;

  int ret = write_based_reg(daq, base, &surf_bq.zero_fir_casc, bq->B) ||
  write_based_reg(daq, base, &surf_bq.zero_fir_casc, bq->A) ||
  write_based_reg(daq, base, &surf_bq.zero_fir_casc, bq->B) ||
  write_based_reg(daq, base, &surf_bq.zero_fir_casc, bq->A) ||
  write_based_reg(daq, base, &surf_bq.zero_fir_casc, bq->B) ||
  write_based_reg(daq, base, &surf_bq.zero_fir_casc, bq->A) ||
  write_based_reg(daq, base, &surf_bq.zero_fir_casc, bq->B) ||
  write_based_reg(daq, base, &surf_bq.zero_fir_casc, bq->A) ||

  write_based_reg(daq, base, &surf_bq.pole_f_fir_casc, bq->D_FF) ||
  write_based_reg(daq, base, &surf_bq.pole_f_fir_casc, bq->F[0]) ||
  write_based_reg(daq, base, &surf_bq.pole_f_fir_casc, bq->F[1]) ||

  write_based_reg(daq, base, &surf_bq.pole_g_fir_casc, bq->E_GG) ||
  write_based_reg(daq, base, &surf_bq.pole_g_fir_casc, bq->G[1]) ||
  write_based_reg(daq, base, &surf_bq.pole_g_fir_casc, bq->G[2]) ||
  write_based_reg(daq, base, &surf_bq.pole_g_fir_casc, bq->G[0]) ||

  write_based_reg(daq, base, &surf_bq.f_in_g_fir, bq->D_FG) ||
  write_based_reg(daq, base, &surf_bq.g_in_f_fir, bq->E_GF) ||

  write_based_reg(daq, base, &surf_bq.pole_iir_casc, bq->C[2]) ||
  write_based_reg(daq, base, &surf_bq.pole_iir_casc, bq->C[3]) ||
  write_based_reg(daq, base, &surf_bq.pole_iir_casc, bq->C[1]) ||
  write_based_reg(daq, base, &surf_bq.pole_iir_casc, bq->C[0]) ||

  write_based_reg(daq, base, &surf_bq.inc_comp_casc, -bq->a1) ||
  write_based_reg(daq, base, &surf_bq.inc_comp_casc, -bq->a2) ||
  write_based_reg(daq, base, &surf_bq.inc_comp_casc, -bq->a1) ||
  write_based_reg(daq, base, &surf_bq.inc_comp_casc, -bq->a2) ||

  write_based_reg(daq, base, &surf_bq.update_control, 0x00001) ||
  write_based_reg(daq, base, &surf_bq.update_control, 0x810000);

//  daq->cfg.debug = old_debug;
  return ret;
}

