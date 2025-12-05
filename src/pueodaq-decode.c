#include "pueodaq.h"


enum e_surf_nicknames
{
  SURF_L0S0,
  SURF_L0S1,
  SURF_L0S2,
  SURF_L0S3,
  SURF_L0S4,
  SURF_L0S5,
  SURF_L0S6,
  SURF_L1S0,
  SURF_L1S1,
  SURF_L1S2,
  SURF_L1S3,
  SURF_L1S4,
  SURF_L1S5,
  SURF_L1S6,
  SURF_L2S0,
  SURF_L2S1,
  SURF_L2S2,
  SURF_L2S3,
  SURF_L2S4,
  SURF_L2S5,
  SURF_L2S6,
  SURF_L3S0,
  SURF_L3S1,
  SURF_L3S2,
  SURF_L3S3,
  SURF_L3S4,
  SURF_L3S5,
  SURF_L3S6,
  SURF_NONE = -1
};

const int surf_buddies[28] =
{
  [SURF_L0S0] =  SURF_L1S0,
  [SURF_L0S1] =  SURF_L0S0,
  [SURF_L0S2] =  SURF_L0S1,
  [SURF_L0S3] =  SURF_L0S2,
  [SURF_L0S4] =  SURF_L0S3,
  [SURF_L0S5] =  SURF_L0S4,
  [SURF_L0S6] =  SURF_NONE,
  [SURF_L1S0] =  SURF_L1S1,
  [SURF_L1S1] =  SURF_L1S2,
  [SURF_L1S2] =  SURF_L1S3,
  [SURF_L1S3] =  SURF_L1S4,
  [SURF_L1S4] =  SURF_L1S5,
  [SURF_L1S5] =  SURF_L0S5,
  [SURF_L1S6] =  SURF_NONE,
  [SURF_L2S0] =  SURF_L2S1,
  [SURF_L2S1] =  SURF_L2S2,
  [SURF_L2S2] =  SURF_L2S3,
  [SURF_L2S3] =  SURF_L2S4,
  [SURF_L2S4] =  SURF_L2S5,
  [SURF_L2S5] =  SURF_L3S5,
  [SURF_L2S6] =  SURF_NONE,
  [SURF_L3S0] =  SURF_L2S0,
  [SURF_L3S1] =  SURF_L3S0,
  [SURF_L3S2] =  SURF_L3S1,
  [SURF_L3S3] =  SURF_L3S2,
  [SURF_L3S4] =  SURF_L3S3,
  [SURF_L3S5] =  SURF_L3S4,
  [SURF_L3S6] =  SURF_NONE
};


pueo_daq_decoded_trigger_meta_t pueo_daq_event_decode_trigger_meta(const pueo_daq_event_header_t *h)
{

  pueo_daq_decoded_trigger_meta_t decoded  = { 0 };
  int isurf = 0;
  for (int l = 0; l < 4; l++)
  {
    for (int s = 0; s < 7; s++)
    {
      int surf_buddy = surf_buddies [7*l+s];

      if (surf_buddy < 0) continue;

      int bl = surf_buddy / 7;
      int bs = surf_buddy % 7;


      uint8_t us =  h->vals.trigger_meta.as_struct.link[l].surf_meta[s];
      uint8_t them =  h->vals.trigger_meta.as_struct.link[bl].surf_meta[bs];

      bool is_l2 = (us & 0xf ) & (them >> 4);

      if (is_l2) decoded.l2_mask |=  (1 << isurf);

      isurf++;

    }
  }

  decoded.soft_flag = !!(h->vals.trigger_meta.as_struct.link[0].tfio_meta);
  decoded.pps_flag = !!(h->vals.trigger_meta.as_struct.link[1].tfio_meta);
  decoded.ext_flag = !!(h->vals.trigger_meta.as_struct.link[2].tfio_meta);

  return decoded;
}








