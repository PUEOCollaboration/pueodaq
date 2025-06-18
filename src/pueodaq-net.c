/** pueodaq Network helpers
 * 
 *   Route table generating heavily based on https://olegkutkov.me/2019/03/24/getting-linux-routing-table-using-netlink/
 *
 * */




#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/rtnetlink.h>
#include <pueodaq-net.h>
#include <errno.h>
#include "pueodaq-test.h"
#include "pueodaq-util.h"



//the maximum number of entries in our routing table (since we statically allocate it)
#ifndef PUEO_ROUTING_TABLE_MAX_SIZE
#define PUEO_ROUTING_TABLE_MAX_SIZE 64
#endif


static int the_ntlink_sock = -1;
static struct sockaddr_nl ntlnk_addr;

void close_ntlink_sock()
{
  if (the_ntlink_sock > 0)
    close(the_ntlink_sock);
  the_ntlink_sock =-1;
}

int ntlink_sock()
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
      ntlnk_addr.nl_family = AF_NETLINK;
      ntlnk_addr.nl_pid = getpid();

      if (bind(the_ntlink_sock, (struct sockaddr *) &ntlnk_addr, sizeof(ntlnk_addr)) < 0)
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

static int robust_recv(int fd, struct msghdr * msg, int flags)
{
  int len;

  do
  {
    len = recvmsg(fd,msg,flags);
  } while (len < 0 && (errno = EINTR || errno==EAGAIN));

  if (len < 0 )
  {
    fprintf(stderr, "Netlink receive failed\n");
    return - errno;
  }

  if (len == 0)
  {
    fprintf(stderr,"no data from netlink\n");
    return -ENODATA;
  }
  return len;
}


static int do_recvmsg(int fd, struct msghdr * msg, char ** answer)
{
    struct iovec *iov = msg->msg_iov;
    char *buf;
    int len;

    iov->iov_base = NULL;
    iov->iov_len = 0;

    len = robust_recv(fd, msg, MSG_PEEK | MSG_TRUNC);

    if (len < 0) {
        return len;
    }


  
    buf = malloc(len);

    if (!buf) {
        perror("malloc failed");
        return -ENOMEM;
    }

    iov->iov_base = buf;
    iov->iov_len = len;

    len = robust_recv(fd, msg, 0);

    if (len < 0) {
        free(buf);
        return len;
    }

    *answer = buf;

    return len;
}




static int fill_route_entry(struct nlmsghdr * h, struct route_entry * e)
{

    struct rtmsg *r = NLMSG_DATA(h);
    int len = h->nlmsg_len;
    struct rtattr * tb[RTA_MAX+1] = {0} ;
    uint32_t table;

    len -= NLMSG_LENGTH(sizeof(*r));

    if (len < 0)
    {
      fprintf(stderr,"Wrong message length\n");
      return -1;
    }

    struct rtattr *rta = RTM_RTA(r);
    while (RTA_OK(rta, len))
    {
      if (rta->rta_type <= RTA_MAX)
      {
        tb[rta->rta_type] = rta;
      }
      rta = RTA_NEXT(rta, len);
    }

    table = r->rtm_table;
    if (tb[RTA_TABLE])
    {
      table =  *(uint32_t *) RTA_DATA(tb[RTA_TABLE]);
    }

    //not the right kind of thing
    if (r->rtm_family != AF_INET && table != RT_TABLE_MAIN)
    {
      return -1;
    }
 
    // do we want to skip /32 addresses? Probably, those are not useful
    if (r->rtm_dst_len == 32)
    {
        return -1;
    }


    e->len = r->rtm_dst_len;

    //we have a dest
    if (tb[RTA_DST])
    {
      e->dest= *((struct in_addr *) RTA_DATA(tb[RTA_DST]));
    }
    else
    {
      e->dest.s_addr = 0;
    }
    //we have a gateway
    if (tb[RTA_GATEWAY])
    {
      e->gw= *((struct in_addr *) RTA_DATA(tb[RTA_GATEWAY]));
    }
    else
    {
      e->gw.s_addr = 0;
    }



    //we have a src
    if (tb[RTA_SRC])
    {
      e->src= *((struct in_addr *) RTA_DATA(tb[RTA_SRC]));
    }
    else
    {
      e->src.s_addr = 0;
    }


     //we have a src (why?!?)
    if (tb[RTA_OIF])
    {
      int idx = *(uint32_t*) RTA_DATA(tb[RTA_OIF]);
      if_indextoname(idx, e->ifname);
    }
    else
    {
      e->ifname[0] = 0;
    }

    return 0;
}


const struct route_entry * get_routing_table(int force_update)
{

  static struct route_entry routing_table[PUEO_ROUTING_TABLE_MAX_SIZE];
  static unsigned routing_table_nentries = 0;

  if (routing_table_nentries && !force_update)
  {
    return routing_table;
  }


  memset(routing_table,0, routing_table_nentries * sizeof(struct route_entry));
  routing_table_nentries = 0;


  int sck = ntlink_sock();
  if (sck < 0) return 0;

  struct
  {
    struct nlmsghdr hdr;
    struct rtmsg msg;
  } nl_req = {0};

  static uint32_t seq = 0;

  nl_req.hdr.nlmsg_type = RTM_GETROUTE;
  nl_req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  nl_req.hdr.nlmsg_len = sizeof(nl_req);
  nl_req.hdr.nlmsg_seq = seq++;
  nl_req.msg.rtm_family = AF_INET;

  ssize_t ns = send(sck,&nl_req, sizeof(nl_req), 0);

  if (ns <0)
  {
    fprintf(stderr,"Could not perform RTM_GETROUTE request");
    return 0;
  }


  // now for the response

  struct sockaddr_nl nladdr;
  struct iovec iov;
  struct msghdr msg = { .msg_name = &nladdr, .msg_namelen = sizeof(nladdr), .msg_iov = &iov, .msg_iovlen = 1};
  char *buf = 0;

  int status = do_recvmsg(sck, &msg, &buf);

  struct nlmsghdr * h = (struct nlmsghdr*) buf;
  int msglen = status;

  while(NLMSG_OK(h, msglen))
  {
    if (h->nlmsg_flags & NLM_F_DUMP_INTR)
    {
       fprintf(stderr,"Routing dump interrupted");
       free(buf);
       return routing_table ;
    }

    if (nladdr.nl_pid !=0)  //not from kernel?
    {
      continue;
    }

    if (h->nlmsg_type == NLMSG_ERROR)
    {
      fprintf(stderr,"netlink reported error");
      free(buf);
      return NULL;
    }

    if (!fill_route_entry(h,&routing_table[routing_table_nentries]))
    {
      routing_table_nentries++;
    }



    h = NLMSG_NEXT(h, msglen);
  }


  free(buf);

  return routing_table;

}



int route_entry_print(const struct route_entry * entry, FILE * f)
{
  int ret = 0;
  char tmpbuf[INET_ADDRSTRLEN];

  // empty route case, print nothing
  if (memallzero(entry, sizeof(struct route_entry)) )
    return 0;

  if (!entry->len)
  {
    ret+= fprintf(f,"default ");
  }
  else
  {
    inet_ntop(AF_INET, &entry->dest, tmpbuf, sizeof(tmpbuf));
    ret+= fprintf(f,"%s/%d ", tmpbuf, entry->len);
  }

  if (entry->gw.s_addr)
  {
    inet_ntop(AF_INET, &entry->gw, tmpbuf, sizeof(tmpbuf));
    ret+= fprintf(f,"via %s ", tmpbuf);
  }

  if (*(entry->ifname))
  {
    ret += fprintf(f,"dev %s ", entry->ifname);
  }

  if (entry->src.s_addr)
  {
    inet_ntop(AF_INET, &entry->src, tmpbuf, sizeof(tmpbuf));
    ret+=fprintf(f,"src %s ", tmpbuf);
  }

  ret+=fprintf(f,"\n");
  return ret;
}

PUEODAQ_TEST(print_routing_table)
{
  const struct route_entry * routing_table = get_routing_table(0);

  PUEODAQ_CHECK(routing_table!= NULL);

  printf("\n");
  int ndefault = 0;
  if (routing_table)
  {
    int ientry = 0;
    while (!memallzero(&routing_table[ientry], sizeof(struct route_entry)))
    {
      if (!routing_table[ientry].len)
      {
        ndefault++;
      }

      route_entry_print(&routing_table[ientry], stdout);
      ientry++;
    }
  }

  PUEODAQ_CHECK(ndefault > 0);
}




bool is_in_subnet(struct in_addr ip, struct in_addr base, uint8_t len)
{
  if (!len) return 1;
  uint32_t naddr =  1 << (32-len);
  return ( ntohl(ip.s_addr -base.s_addr)  < naddr);
}

PUEODAQ_TEST(is_in_subnet_test)
{
  struct in_addr ip;
  struct in_addr base;
  uint8_t len;

  base.s_addr = inet_addr("10.1.0.0");
  ip.s_addr = inet_addr("10.1.1.42");
  len = 16;
  PUEODAQ_CHECK(is_in_subnet(ip,base,len));

  len = 24;
  PUEODAQ_CHECK(!is_in_subnet(ip,base,len));

  ip.s_addr = inet_addr("10.1.0.42");
  PUEODAQ_CHECK(is_in_subnet(ip,base,len));

  base.s_addr = inet_addr("0.0.0.0");
  len = 0;
  PUEODAQ_CHECK(is_in_subnet(ip,base,len));
}


const struct route_entry *  get_route_for_addr(const char * ip, const struct route_entry * table)
{
  table = table ?: get_routing_table(0);
  struct in_addr the_ip;
  inet_pton(AF_INET,ip,&the_ip);

  int best_entry=-1;
  int best_entry_len=-1;

  int ientry = 0;
  while (!memallzero(&table[ientry], sizeof(struct route_entry) ) )
  {
    uint8_t len = table[ientry].len;
    if (is_in_subnet(the_ip, table[ientry].dest, len) && len  > best_entry_len)
    {
      best_entry_len = len;
      best_entry = ientry;
    }

    ientry++;
  };

  if (best_entry < 0)
  {
    return NULL;
  }

   return &table[best_entry];
}

PUEODAQ_TEST(best_route_entry)
{
  struct route_entry fake_table[] =
  {
    {
      .ifname = "eth0",
      .gw = {inet_addr("192.168.1.1")},
      .dest= {0},
      .src = {0},
      .len = 0
    },
    {
      .ifname = "eth1",
      .gw = {0},
      .dest = {inet_addr("10.1.0.0")},
      .src = {0},
      .len = 16
    },
    {0}
  };

  const struct route_entry * e = get_route_for_addr("10.1.5.3", fake_table);
  PUEODAQ_CHECK(e && e->ifname && !strcmp(e->ifname,"eth1"));
  e = get_route_for_addr("10.0.5.3", fake_table);
  PUEODAQ_CHECK(e && e->ifname && !strcmp(e->ifname,"eth0"));
}

PUEODAQ_TEST(route_entry_padding_initialization)
{
  struct route_entry e = {0};
  PUEODAQ_CHECK(memallzero(&e,sizeof(e)));
}




int add_route(const struct route_entry *e)
{
  if (!e) return 0;

  int sck = ntlink_sock();

  if (sck < 0) return -1;

  struct
  {
    struct nlmsghdr h;
    struct rtmsg msg;
    char buf[512];
  } req =
  {
    .h =
    {
      .nlmsg_len   = NLMSG_LENGTH(sizeof(struct rtmsg)),
      .nlmsg_type  = RTM_NEWROUTE,
      .nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL,
    },
    .msg  =
    {
      .rtm_table    = RT_TABLE_MAIN,
      .rtm_type     = RTN_UNICAST,
      .rtm_family   = AF_INET,
      .rtm_scope    = RT_SCOPE_LINK,
      .rtm_dst_len  = e->len ,
      .rtm_protocol = RTPROT_BOOT
    },
    .buf = {0}
  };

#define ADD_ATTR(type, data, data_size)\
  do\
  {\
    struct rtattr * rta = (struct rtattr*) ( ((uint8_t*) &req.h)+ NLMSG_ALIGN(req.h.nlmsg_len)); \
    int rta_len = RTA_LENGTH(data_size); \
    if (NLMSG_ALIGN(req.h.nlmsg_len)+ RTA_ALIGN(rta_len) <= sizeof(req))\
    {\
       rta->rta_type = type; \
       rta->rta_len = rta_len; \
       if (data_size) \
       {\
         memcpy(RTA_DATA(rta), data, data_size);\
       }\
      req.h.nlmsg_len = NLMSG_ALIGN(req.h.nlmsg_len) + RTA_ALIGN(rta_len); \
    }\
  } while(0)

  if ( *e->ifname)
  {
    int if_idx = if_nametoindex(e->ifname);
    if (if_idx)
    {
      ADD_ATTR(RTA_OIF, &if_idx, sizeof(if_idx));
    }
    else
    {
      fprintf(stderr,"bad interface: %s\n", e->ifname);
      return -1;
    }
  }

  if (e->gw.s_addr)
    ADD_ATTR(RTA_GATEWAY, &e->gw, sizeof(e->gw));

  if (e->dest.s_addr)
    ADD_ATTR(RTA_DST, &e->dest, sizeof(e->dest));

  if (e->src.s_addr)
    ADD_ATTR(RTA_SRC, &e->src, sizeof(e->src));


#undef ADD_ATTR
  return send(sck, &req, sizeof(req), 0) != sizeof(req);
}



