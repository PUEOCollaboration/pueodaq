/** Just runs the turfid command as a test */ 


#include "turfeth.h" 
#include <sys/types.h> 
#include <arpa/inet.h> 
#include <sys/socket.h> 
#include <stdio.h> 

const char * ip = "10.68.65.81"; 

int main(int nargs, char ** args) 
{

  if (nargs > 1 )
  {
    ip = args[1]; 
  }

  int sck = socket(AF_INET, SOCK_DGRAM, 0); 

  if (sck < 0) 
  {
    fprintf(stderr,"Could not open socket\n"); 
    return 1; 
  }

  turf_ctl_t idreq = {.BIT = {.COMMAND = TURF_ID_COMMAND}}; 

  struct sockaddr_in dest = {.sin_family = AF_INET, .sin_port = htons(21603)}; 
  inet_pton(AF_INET, ip, &dest.sin_addr); 

  if (connect(sck, (struct sockaddr*) &dest, sizeof(dest)) < 0) 
  {
    fprintf(stderr,"Connection error\n"); 
    return 1; 
  }

  if (send(sck, &idreq, sizeof(idreq),0) < 0) 
  {
    fprintf(stderr,"Send error...\n"); 
    return 1; 
  }

  uint64_t repl; 

  if (recv(sck, &repl, sizeof(repl),0) < sizeof(repl))
  {
    fprintf(stderr,"Didn't get 8 bytes...\n"); 
    return 1; 
  }

  printf("cmd: %x, id: %llx\n", repl & 0xffff, (repl >> 16)); 

  return 0; 

}


