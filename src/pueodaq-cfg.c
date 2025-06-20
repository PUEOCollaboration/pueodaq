#ifndef _PUEODAQ_CFG_H
#define _PUEODAQ_CFG_H

#include "pueodaq-net.h"
#include "pueodaq.h"
#include <unistd.h>
#include "ifaddrs.h"
#include <string.h>
#include <net/if.h>
#include <sys/ioctl.h>




#include "pueodaq-test.h"
PUEODAQ_TEST(daq_config_validate)
{
  pueo_daq_config_t cfg = {  PUEO_DAQ_CONFIG_DFLT, .fragment_size = 1400 } ;

  struct in_addr our_ip;
  PUEODAQ_CHECK ( !pueo_daq_config_validate (&cfg, stdout, &our_ip));
  printf("%s\n",inet_ntoa(our_ip));

}



int pueo_daq_config_validate(const pueo_daq_config_t * cfg, FILE * out, struct in_addr * our_ip)
{

  int ret = 0;

  int sock = 0;
  if (cfg->eth_device && strlen(cfg->eth_device) >= IFNAMSIZ)
  {
    fprintf(out,"%s is too long... (should be max %d)\n", cfg->eth_device, IFNAMSIZ-1);
    goto end;
  }

  // Verify the network configuration is sane...
  // Open a netlink socket
  sock = ntlink_sock();

  // get the route for the turf

  const struct route_entry * e = get_route_for_addr(cfg->turf_ip_addr, 0);
  printf("%s\n", e->ifname);

  if (cfg->eth_device && strcmp(cfg->eth_device, e->ifname))
  {
    fprintf(out,"The kernel is routing the TURF IP (%s) to %s, but you specified the device %s. Trying to change route...\n", cfg->turf_ip_addr, e->ifname, cfg->eth_device);

    struct route_entry set = { .len = 31} ;
    inet_aton(cfg->turf_ip_addr, &set.dest);
    memcpy(set.ifname, cfg->eth_device, IFNAMSIZ);
    add_route(&set);
    e = get_route_for_addr(cfg->turf_ip_addr,0);
    if (cfg->eth_device && strcmp(cfg->eth_device, e->ifname))
    {
        fprintf(out,"The kernel is is still routing the TURF IP (%s) to %s, but you specified the device %s. Bailing...\n", cfg->turf_ip_addr, e->ifname, cfg->eth_device);
        goto end;
    }
  }


  struct ifreq ifr;
  const char * eth_device = cfg->eth_device ?: e->ifname;
  memcpy(ifr.ifr_name, eth_device, IFNAMSIZ);

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

  //check that we are on the same subnet as the turf
  struct in_addr turf_inaddr;
  inet_aton(cfg->turf_ip_addr, &turf_inaddr);

  struct ifaddrs * ifaddr = NULL;
  getifaddrs(&ifaddr);

  bool found_valid_ip = false;
  for (struct ifaddrs * ifa = ifaddr;  ifa != NULL ; ifa = ifa->ifa_next )
  {
    if (strcmp(ifa->ifa_name,eth_device)) continue; //skip wrong device
    if (ifa->ifa_addr->sa_family != AF_INET) continue; //skip non ipv4
    struct sockaddr_in * sin = (struct sockaddr_in*) ifa->ifa_addr;

    if (is_in_subnet(sin->sin_addr, turf_inaddr, cfg->turf_subnet_len))
    {
      if (our_ip) *our_ip = sin->sin_addr;
      printf("Found valid IP %s\n", inet_ntoa(sin->sin_addr));
      found_valid_ip = true;
      break;
    }
  }

  freeifaddrs(ifaddr);

  if (!found_valid_ip)
  {
    fprintf(stderr,"Could not find any valid ip's on %s in the TURF subnet (%s/%hhu)\n", eth_device, cfg->turf_ip_addr, cfg->turf_subnet_len);
  }

end:
  close(sock);
  return ret ;

}



#endif
