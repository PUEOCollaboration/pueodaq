#include "pueodaq.h"
#include "daqregs.h"
#include "pueodaq-private.h"
#include <stdlib.h>

void pueo_daq_check_corruption(pueo_daq_t * daq,
                               const pueo_daq_event_data_t * event,
                               uint16_t level_threshold,
                               pueo_daq_corruption_detector_output_t * out)
{

  if (!out)
  {
    fprintf(stderr,"pueo_daq_check_corruption needs to store its result somewhere!\n");
    return;
  }

  int chan = 0;
  for (int itfio = 0; itfio < NTFIO; itfio++)
  {
    for (int isurf = 0; isurf < NSURFSLOTS; isurf++)
    {

        for (int ichan = 0; ichan < 8; ichan++)
        {
          //this should be a ramp
          if (!daq->census.turfio[itfio].surfid[isurf])
          {
             out->turfio[itfio].surf[isurf].is_present = false;

            //check for ramp decrements
            for (int i = 1; i < PUEODAQ_NSAMP; i++)
            {
              if (event->waveform_data[chan][i] < event->waveform_data[chan][i-1])
              {
                 out->turfio[itfio].surf[isurf].decrement_counter++;
              }
            }
          }
          else
          {
           out->turfio[itfio].surf[isurf].is_present = true;
           out->turfio[itfio].surf[isurf].n_above_level = 0;

           //check for values above level
            for (int i = 0; i < PUEODAQ_NSAMP; i++)
            {
              if (abs(event->waveform_data[chan][i])  > level_threshold)
              {
                out->turfio[itfio].surf[isurf].n_above_level++;
              }
            }

          }

          chan++;
      }
    }
  }

}
