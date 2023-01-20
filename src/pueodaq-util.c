#include "pueodaq-util.h" 
#include "pueodaq-test.h" 
#include <stdint.h> 
#include <string.h> 



//using rusty's trick 
int memallzero(const void * mem, size_t len) 
{

#define START_BYTES 16 

  const uint8_t * bytes = mem; 

  size_t i = 0; 
  for (i = 0; i < START_BYTES; i++) 
  {
    if (!len) return 1; 
    if (*bytes) return 0; 

    bytes++; 
    len--; 
  }
  //now we know the first START_BYTES of mem are 0 if we made it this far, let's use memcmp for the rest
  return !memcmp(mem, bytes, len); 
}


PUEODAQ_TEST(test_memallzero) 
{
  uint8_t buf[128] = {0}; 

  PUEODAQ_CHECK(memallzero(buf,sizeof(buf)));

  buf[64]=0x42; 

  PUEODAQ_CHECK(!memallzero(buf,sizeof(buf)));

}


