#ifndef _PUEODAQ_NET_H
#define _PUEODAQ_NET_H

#include <stdint.h> 
#include <arpa/inet.h>
#include <net/if.h>

//Internal helpers for networking stuff 

//get a netlink socket (may be already open)
int ntlink_sock(); 

//close netlink socket (if it's open) 
void close_ntlink_sock(); 

struct route_entry
{
  char ifname[IFNAMSIZ]; 
  struct in_addr dest; 
  struct in_addr gw; 
  struct in_addr src; 
  uint8_t len; // 0 for default route, otherwise the suffix 
}; 



// query routing table for the route to ip. 
// ip is a an ipv4 string
// table is a an array of route_entries ending with an all 0 entry, pass 0 to automatically call get_routing_table(0) 
// returns a pointer to a route_entry. Note that this may be invalidated at some point in the future, so you should copy it if you need to persist it. 

const struct route_entry *  get_route_for_addr(const char * ip, const struct route_entry * table); 

// get the system routing table. Pass non-zero to force an update (instead of using cached table), which may invalidate old route_entries if they go away 
// returns a pointer to the beginning of a route_entry array that is terminated by an all-zero entry 
const struct route_entry * get_routing_table(int force_update); 



// set a routing entry( equivalent to ip route add dest/mask via gw dev ifname) 
int add_route(const struct route_entry *e); 






#endif
