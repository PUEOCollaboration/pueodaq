
#include "pueodaq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

volatile int stop = 0;

void handler(int signum)
{
  stop = 1;
}

double interval = 1;
uint8_t turfio_mask = 0;
int debug = 1;
uint8_t nthreads = 1;
int max_in_flight =32;
const char * outdir = "/tmp";

int ready(pueo_daq_t * daq, uint32_t idx)
{
  printf("Event %u ready\n", idx);

  pueo_daq_event_data_t  * d = calloc(1,sizeof(pueo_daq_event_data_t));

  pueo_daq_get_event(daq, d);
  char fname[512];
  sprintf(fname,"%s/fakedaq_%05d.dat", outdir, idx);
  FILE * f  = fopen(fname,"w");
  int nb =fwrite(d, sizeof(pueo_daq_event_data_t), 1, f);
  fclose(f);
  free(d);
  printf("%d bytes written to %s\n", nb*sizeof(pueo_daq_event_data_t), fname);
}

int main (int nargs, char ** args)
{

  if (nargs > 1)
  {

    for (int  i = 1; i < nargs; i++)
    {
      bool last = (i == nargs-1);
      if (!strcmp(args[i],"-I") && !last)
      {
        float maybe_interval = atof(args[++i]);
        if (maybe_interval > 0) interval = maybe_interval;
      }
      else if (!strcmp(args[i],"-T") && !last)
      {
        turfio_mask = strtol(args[++i], NULL, 0);
      }
      else if (!strcmp(args[i],"-t") && !last)
      {
        int maybe_nthreads = atoi(args[++i]);
        if (maybe_nthreads  > 0) nthreads = maybe_nthreads;
      }
      else if (!strcmp(args[i],"-M") && !last)
      {
        int maybe_max = atoi(args[++i]);
        if (maybe_max > 0) max_in_flight = maybe_max;

      }
      else if (!strcmp(args[i],"-o") && !last)
      {
        outdir = args[++i];
      }
      else if (!strcmp(args[i],"-d") && !last)
      {
        debug = atoi(args[++i]);
      }
    }

  }

  printf("Using interval %f, turfio_mask 0x%hhx\n", interval, turfio_mask);
  pueo_daq_config_t cfg = { PUEO_DAQ_CONFIG_DFLT, .fragment_size = 1024, .debug = debug, .n_recvthreads =nthreads, .max_in_flight = max_in_flight, .turfio_mask = turfio_mask };

  signal(SIGINT, handler);

  pueo_daq_t * daq = pueo_daq_init(&cfg);

  if (!daq) return 1;

  pueo_daq_register_ready_callback(daq, ready);

  pueo_daq_start(daq);

  pueo_daq_dump(daq,stdout, 0);
  while(!stop)
  {
    pueo_daq_soft_trig(daq);
    usleep(1e6*interval);
  }
  pueo_daq_dump(daq,stdout, 0);
  pueo_daq_stop(daq);
  pueo_daq_destroy(daq);
  return 0;
}
