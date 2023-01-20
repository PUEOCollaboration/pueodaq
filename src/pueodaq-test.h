/*
*
* pueodaq-test.h  
*
* This file is part of pueodaq. 
* 
* This is a a vendored version of my (Cosmin's)  crappy in-line header-only C unit test "framework." 
*
* This is mostly a proof of concept experiment to use linker sections for inline
* tests, which is kind of nice actually.. It is horribly unportable and a
* complete hack, but  we'll keep it for now? 
*
* Contributing Authors:  cozzyd@kicp.uchicago.edu
*
* Copyright (C) 2022-  PUEO Collaboration
** 
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <https://www.gnu.org/licenses/>.
* 
*
*/

#ifndef _PUEODAQ_TEST_H
#define _PUEODAQ_EST_H
#include <stdio.h> 


#define PUEODAQTEST_SECTION __attribute__((section("pueodaqtest"))) __attribute__((__used__))


struct pueodaq_test_q
{
  const char * name; 
  void (*fn)(); 
}; 


#define PUEODAQ_NAMED_TEST(what,nm) \
static void what(); \
static struct pueodaq_test_q what##_s = { .name = nm, .fn = &what }; \
static struct pueodaq_test_q * what##_s_ptr PUEODAQTEST_SECTION = &what##_s;  \
void what()

#define PUEODAQ_TEST(what) PUEODAQ_NAMED_TEST(what,#what) 


extern struct pueodaq_test_q* __start_pueodaqtest;
extern struct pueodaq_test_q* __stop_pueodaqtest;

#ifndef PUEODAQTEST_MAIN
extern int pueodaq_test_npass;
extern int pueodaq_test_nfail;
#else
int pueodaq_test_npass;
int pueodaq_test_nfail;
#endif


#define PUEODAQ_FAIL(file,line,expr) printf("\n     failing check:  %s (%s:%d) ",expr,  file ,line)

#define PUEODAQ_CHECK(condition) \
 do \
 {\
   if (condition) pueodaq_test_npass++;\
   else\
   {\
     PUEODAQ_FAIL(__FILE__,__LINE__, #condition);\
     pueodaq_test_nfail++; \
   }\
 }\
 while (0); 


#ifdef PUEODAQTEST_MAIN
int main(int nargs, char ** args) 
{
  (void) nargs; 
  (void) args; 

  struct pueodaq_test_q ** q =  &__start_pueodaqtest; 
  struct pueodaq_test_q ** qend  =  &__stop_pueodaqtest; 
  int itest = 0; 
  int npass = 0;
  int ntotal = 0; 
  int ntotal_parts = 0;
  printf("Running tests...\n---\n");
  while (q!=qend) 
  {
    int current_npass = pueodaq_test_npass; 
    int current_nfail = pueodaq_test_nfail; 
    printf("[%d] %s:\n", itest++, (*q)->name); 
    (*q)->fn(); 
    int total_parts = pueodaq_test_npass + pueodaq_test_nfail - current_npass - current_nfail; 
    if (!total_parts) 
    {
      printf("  !WARNING: test \"%s\" has no check! Skipping.", (*q)->name); 
    }
    int failed = pueodaq_test_nfail - current_nfail;
    npass+= !failed; 
    ntotal_parts+= total_parts; 
    ntotal++; 
    
    printf("\n  %s (%d/%d parts passed) \n-----\n", failed ? "FAILED" : "PASSED", total_parts - failed, total_parts); 

    q++; 
  } 
  printf ("SUMMARY:  %d/%d tests passed (%d/%d parts) \n", npass, ntotal, pueodaq_test_npass, ntotal_parts); 
  return pueodaq_test_nfail; 
}
#endif
  
  

#endif 


