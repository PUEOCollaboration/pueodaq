/** Network helpers */

#include <stdlib.h>
#include <stdint.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/socket.h> 


static int the_ntlink_sock = -1;
static struct sockaddr_nl ntlnk_addr; 
static int nl_seq = 0; 

static void close_ntlink_sock()
{
  if (the_ntlink_sock > 0)
    close(the_ntlink_sock; 
  the_ntlink_sock =-1; 
}

static int ntlink_sock() 
{
  if (the_ntlink_sock < 0) 
  {
    the_ntlink_sock  = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE); 
    if (the_ntlink_sock < 0) 
    {
       fprintf(stderr,"Could not open netlink sock!\n"); 
       return -1; 
    }
    else 
    {
      ntlink_addr.nl_family = AF_NETLINK; 
      ntlink_addr.nl_pid = getpid(); 

      if (bind(the_ntlink_sock, (struct sockaddr *) &ntlink_addr, sizeof(ntlink_addr) < 0))
      {
        fprintf(stderr,"Could not bind to netlink sock\n"); 
        close_ntlink_sock(); 
      }
      else
      {
        atexit(close_ntlink_sock); 
      }
    }
  }
  return the_ntlink_sock; 
}

int get_route_for_addr(const char * ip, const char ** ifname, const char ** gw)
{

  int sck = ntlink_sock(); 
  if (sck < 0) return -1; 

  struct 
  {
    struct nlmsghdr hdr; 
    struct rtmsg msg; 
  } nl_req; 

  nl_req.hdr.nlmsg_type = RTM_GETROUTE;
  nl_req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP; 
  nl_req.hdr.nlmsg_len = sizeof(nl_req); 




}
void set_route_for_addr(const char * ip, const char * ifname, const char *mask)
{
}



