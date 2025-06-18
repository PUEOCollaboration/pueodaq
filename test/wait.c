
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <unistd.h>

int amt = 100;
volatile int quit = 0;

void handler(int sig)
{
  (void) sig;
  quit = 1;

}

int main(int nargs, char ** args)
{

  if (nargs > 1) amt = atoi(args[1]);

  struct timespec start;
  struct timespec end;
  unsigned long long counter = 0;
  signal(SIGINT,handler);
  struct rusage usage_start;
  getrusage(RUSAGE_SELF, &usage_start);
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (!quit)
  {
    usleep(amt);
    counter++;
  }
  clock_gettime(CLOCK_MONOTONIC, &end);
  struct rusage usage_end;
  getrusage(RUSAGE_SELF, &usage_end);
  double elapsed =  end.tv_sec - start.tv_sec + 1e-9 * (end.tv_nsec - start.tv_nsec);
  double expected = amt * 1e-6 * counter;
  printf("elapsed, %f, counter: %d (%f expected, extra_sleep_ratio: %f)\n", elapsed, counter, expected, elapsed/expected);
  double utime = (usage_end.ru_utime.tv_sec + 1e-6 * usage_end.ru_utime.tv_usec) - (usage_start.ru_utime.tv_sec + 1e-6 * usage_start.ru_utime.tv_usec);
  double stime = (usage_end.ru_stime.tv_sec + 1e-6 * usage_end.ru_stime.tv_usec) - (usage_start.ru_stime.tv_sec + 1e-6 * usage_start.ru_stime.tv_usec);
  printf("     User cpu time: %f (%f%%),  Sys cpu time: %f (%f%%), Total: %f (%f%%)\n",
      utime, 100*utime / elapsed, stime, 100*stime/elapsed, utime+stime, 100*(utime+stime)/elapsed);
  return 0;
}
