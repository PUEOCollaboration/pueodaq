
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

int ready(pueo_daq_t * daq, uint32_t idx)
{
  printf("Event %u ready\n", idx);

  pueo_daq_event_data_t  * d = calloc(1,sizeof(*d));

  pueo_daq_get_event(daq, d);
  char fname[128];
  sprintf(fname,"/tmp/fakedaq_%05d.dat", idx);
  FILE * f  = fopen(fname,"w");
  fwrite(&d, sizeof(d), 1, f);
  fclose(f);
  free(d);
  printf("Written to %s\n", fname);
}

int main (int nargs, char ** args)
{

  pueo_daq_config_t cfg = { PUEO_DAQ_CONFIG_DFLT, .fragment_size = 1024, .debug = (nargs == 2 && !strcmp(args[1],"-d")), .n_recvthreads =1, .max_in_flight = 32 };

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
