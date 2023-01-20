#ifndef _PUEODAQ_CFG_H
#define _PUEODAQ_CFG_H

#include "pueodaq-net.h" 
#include "pueodaq.h" 
#include <unistd.h> 
#include <string.h> 
#include <net/if.h>
#include <sys/ioctl.h>




int pueo_daq_config_validate(const pueo_daq_config_t * cfg, FILE * out) 
{


  // Verify the network configuration is sane... 
  // Open a netlink socket
  int sock = ntlink_sock(); 

  // get the route for the turf

  const struct route_entry * e = get_route_for_addr(cfg->turf_ip_addr, 0); 

  if (cfg->eth_device && strcmp(cfg->eth_device, e->ifname))
  {
    fprintf(out,"The kernel is routing the TURF IP (%s) to %s, but you specified the device %s. Trying to change route...\n", cfg->turf_ip_addr, e->ifname, cfg->eth_device); 

    //set_route_for_addr
  }

  struct ifreq ifr; 
  strncpy(ifr.ifr_name, cfg->eth_device, IFNAMSIZ-1); 
  int ret = 0; 

  //check the MTU 
  if (ioctl(sock, SIOCGIFMTU, &ifr) )
  {
    if (out) fprintf(out,"Could not query MTU (is %s a real device?)!!!\n", cfg->eth_device); 
    ret = -1; 
    goto end; 

  }
  if (ifr.ifr_mtu < cfg->fragment_size + 8) 
  {
    if (out) 
    {
      fprintf(out, "MTU of %d too small for fragment size of %d, trying to increase\n", ifr.ifr_mtu, cfg->fragment_size); 
    }

    //I suppose we could try to change it? 
    ifr.ifr_mtu = cfg->fragment_size+8; 
    if (ioctl(sock, SIOCSIFMTU, &ifr))
    {
      if (out) fprintf(out, "Could not set MTU to %d", ifr.ifr_mtu); 
      ret = -1; 
      goto end; 
    }
  }

  // TODO check that the device has an ip on the same subnet as the turf? 
  
  // TODO check route? 



end: 
  close(sock); 
  return ret ; 

}



#endif
