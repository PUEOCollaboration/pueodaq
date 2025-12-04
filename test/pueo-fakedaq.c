
#include "pueodaq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

volatile int stop = 0;

void handler(int signum)
{
  stop = 1;
}

double interval = 0;
double stats_interval =5;
uint8_t turfio_mask = 0;
int debug = 1;
uint8_t nthreads = 1;
bool throw_away = false;
int max_in_flight =32;
const char * outdir = "/tmp";
int fraglen = 1024;
int frag_src_mask = 0x3f;
int last_idx = 0;
int maybe_thresholds = -1;
bool enable_pps = false;
int pps_offset = 0;

int ready(pueo_daq_t * daq, uint32_t idx)
{

  last_idx = idx;
  if (throw_away)
  {
    pueo_daq_get_event(daq, NULL);
    printf("%d bytes from ev %d sent to the bitbucket\n", sizeof(pueo_daq_event_data_t), idx);
    return 0;
  }

  pueo_daq_event_data_t  * d = calloc(1,sizeof(pueo_daq_event_data_t));
  pueo_daq_get_event(daq, d);
  printf("   Got event number %u (rcv idx %u)\n", d->header.vals.event_number, idx);
  char fname[512];
  sprintf(fname,"%s/fakedaq_%05d.dat", outdir, idx);
  FILE * f  = fopen(fname,"w");
  int nb =fwrite(d, sizeof(pueo_daq_event_data_t), 1, f);
  fclose(f);
  free(d);
  printf("%d bytes written to %s\n", nb*sizeof(pueo_daq_event_data_t), fname);
  return 0;
}

void usage()
{
  printf("pueo-fakedaq [-I SOFTTRIGINTERVAL=1.0] [-T TURFIOMASK=0x0] [-L FRAGLEN=1024] [-t NUMRDRTHREADS=1] [-p offset enable pps with offset] [-H set all thresholds to value] [-M MAXINFLIGHT=32] [-o OUTDIR=/tmp] [-d DEBUGLEVEL=1] [-STATSINTERVAL = 5] [-h] [-0]\n"); 
  printf("   -0 means throw everything away (good for benchmarks)\n");

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
      else if (!strcmp(args[i],"-S") && !last)
      {
        float maybe_interval = atof(args[++i]);
        if (maybe_interval > 0) stats_interval = maybe_interval;
      }
      else if (!strcmp(args[i],"-T") && !last)
      {
        turfio_mask = strtol(args[++i], NULL, 0);
      }
      else if (!strcmp(args[i],"-H") && !last)
      {
        maybe_thresholds = strtol(args[++i], NULL, 0);
      }
      else if (!strcmp(args[i],"-L") && !last)
      {
        fraglen = strtol(args[++i], NULL, 0);
        if (fraglen < 1024) fraglen = 1024;
      }
      else if (!strcmp(args[i],"-t") && !last)
      {
        int maybe_nthreads = atoi(args[++i]);
        if (maybe_nthreads  > 0) nthreads = maybe_nthreads;
      }
      else if (!strcmp(args[i],"-p") && !last)
      {
          enable_pps = true;
          pps_offset = atoi(args[++i]);
      }
      else if (!strcmp(args[i],"-F") && !last)
      {
        frag_src_mask = strtol(args[++i],NULL,0);
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
      else if (!strcmp(args[i],"-0"))
      {
	throw_away = true;
      }
      else if (!strcmp(args[i],"-d") && !last)
      {
        debug = atoi(args[++i]);
      }
      else if (!strcmp(args[i],"-h"))
      {
        usage();
        return 0;
      }
      else
      {
        usage();
        return 1;
      }
    }

  }

  printf("Using interval %f, turfio_mask 0x%hhx\n", interval, turfio_mask);
  pueo_daq_config_t cfg = { PUEO_DAQ_CONFIG_DFLT, .fragment_size = fraglen, .debug = debug, .n_recvthreads =nthreads, .max_in_flight = max_in_flight, .trigger = {.turfio_mask = turfio_mask }, .frag_src_mask = frag_src_mask };

  signal(SIGINT, handler);

  pueo_daq_t * daq = pueo_daq_init(&cfg);

  if (!daq) return 1;

  pueo_daq_register_ready_callback(daq, ready);

  pueo_daq_pps_setup(daq,enable_pps,pps_offset);
  pueo_daq_enable_rf_readout(daq,true,false);

  if (maybe_thresholds > 0)
  {
    static uint32_t thresholds[PUEO_L1_BEAMS];
    static uint32_t pseudo_thresholds[PUEO_L1_BEAMS];
    for (int i = 0; i < PUEO_L1_BEAMS; i++)
    {
      thresholds[i] = maybe_thresholds;
      pseudo_thresholds[i] = maybe_thresholds*0.8;
    }

    for (int turfio = 0; turfio < 4; turfio++)
    {
      for (int surf = 0; surf < 7; surf++)
      {
        pueo_daq_set_L1_thresholds(daq, turfio, surf, thresholds, pseudo_thresholds);
      }
    }
  }
  pueo_daq_set_L2_mask(daq,0x3ffffff);

  pueo_daq_start(daq);

  pueo_daq_dump(daq,stdout, debug > 0 ? PUEODAQ_DUMP_INCLUDE_L1: 0);
  int count = 0;
  struct timespec start;
  clock_gettime(CLOCK_MONOTONIC, &start);
  struct timespec last_stats;
  memcpy(&last_stats, &start, sizeof(start));
  while(!stop)
  {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (interval)
    {
      printf("Sending soft trig %d\n", count++);
      pueo_daq_soft_trig(daq);
      usleep(1e6*interval);
    }
    else
    {
      usleep(10e6);
    }

    if (now.tv_sec - last_stats.tv_sec + 1e-9 * (now.tv_nsec - last_stats.tv_nsec)  > stats_interval)
    {
       memcpy(&last_stats, &now, sizeof(now));
       pueo_daq_dump(daq,stdout,debug > 0 ? PUEODAQ_DUMP_INCLUDE_L1 : 0);
    }
  }

  struct timespec stop;
  clock_gettime(CLOCK_MONOTONIC, &stop);
  pueo_daq_dump(daq,stdout, 0);
  pueo_daq_stop(daq);
  pueo_daq_destroy(daq);
  double T = stop.tv_sec - start.tv_sec + 1e-9 * (start.tv_nsec - stop.tv_nsec);
  printf("Ran for %f seconds, sent triggers at %f Hz, got events at %f Hz\n", T, count/T, last_idx/T);

  return 0;
}
