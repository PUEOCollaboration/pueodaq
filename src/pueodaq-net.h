#ifndef _PUEODAQ_NET_H
#define _PUEODAQ_NET_H

//Internal helpers for networking stuff 


// query routing table for the route to ip. If ifname/gw are not null, they are filled. They should be
// long enough (16 chars?) :)
int get_route_for_addr(const char * ip, char * ifname[16], char * gw[16]) ; 


// set a routing entry( equivalent to ip route add ip/mask via gw dev ifname) 
int set_route_for_addr(const char * ip, const char * gw, const char * ifname, uint8_t mask)



#endif
