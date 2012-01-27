/* Description: testcase for netlink sockets migration.
 * e.g.
 *  ip rule show
 *  ip rule add
 *  ip rule show
 *  ip rule del
 * in a loop
 */
#include <asm/types.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <string.h>
#include <linux/rtnetlink.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "zdtmtst.h"

#undef DEBUG
//#define DEBUG

const char *test_doc    = "Netlink socket loop";
const char *test_author = "Andrew Vagin (avagin@parallels.com)";

//buffer to hold the RTNETLINK request
struct {
	struct nlmsghdr	nl;
	struct rtmsg	rt;
	char		buf[8192];
} req;

// variables used for
// socket communications
int fd;
struct sockaddr_nl la;
struct sockaddr_nl pa;
struct msghdr msg;
struct iovec iov;
int rtn;
// buffer to hold the RTNETLINK reply(ies)
char buf[8192];
char dsts[24] = "192.168.0.255";
int pn = 32;//network prefix

// RTNETLINK message pointers & lengths
// used when processing messages
struct nlmsghdr *nlp;
int nll;
struct rtmsg *rtp;
int rtl;
struct rtattr *rtap;

int send_request();
int recv_reply();
int form_request_add();
int form_request_del();
int read_reply();
typedef int (*cmd_t)();
#define CMD_NUM 2
cmd_t cmd[CMD_NUM]={form_request_add, form_request_del};


int main(int argc, char *argv[])
{
	int ret=0;
	int i;

	test_init(argc, argv);

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd<0){
		err("socket: %m ");
		goto out;
	}
	// setup local address & bind using
	// this address
	bzero(&la, sizeof(la));
	la.nl_family = AF_NETLINK;
	la.nl_pid = getpid();
	if (bind(fd, (struct sockaddr*) &la, sizeof(la))){
		err("bind failed: %m ");
		ret=-1;
		goto out;
	}
	//Preperation:
	form_request_del();
	send_request();
	recv_reply();

	test_daemon();

	while (test_go()){
		for (i=0;i<CMD_NUM;i++){
			cmd[i]();
			if (send_request()<0){
				if ((errno == EINTR) && !test_go())
					goto pass;
				fail("send_request failed");
				goto out;
			};
			if (recv_reply()<0){
				if ((errno == EINTR) && !test_go())
					goto pass;
				fail("RTNETLINK answers: %m");
				goto out;
			};

#ifdef DEBUG
			if (read_reply()<0){
				if ((errno == EINTR) && !test_go())
					goto pass;
				fail("read_reply failed");
				goto out;
			}
#endif
		}
	}

	test_waitsig();
pass:
	pass();

out:
	return 0;
}

int send_request()
{
	// create the remote address
	// to communicate
	bzero(&pa, sizeof(pa));
	pa.nl_family = AF_NETLINK;
	// initialize & create the struct msghdr supplied
	// to the sendmsg() function
	bzero(&msg, sizeof(msg));
	msg.msg_name = (void *) &pa;
	msg.msg_namelen = sizeof(pa);
	// place the pointer & size of the RTNETLINK
	// message in the struct msghdr
	iov.iov_base = (void *) &req.nl;
	iov.iov_len = req.nl.nlmsg_len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	// send the RTNETLINK message to kernel
	rtn = sendmsg(fd, &msg, 0);
	if (rtn<0){
		err("sendmsg failed: %m");
		return -1;
	}
	return 0;
}
int recv_reply()
{
	char *p;
	// initialize the socket read buffer
	bzero(buf, sizeof(buf));
	p = buf;
	nll = 0;
	// read from the socket until the NLMSG_DONE is
	// returned in the type of the RTNETLINK message
	// or if it was a monitoring socket
	while(1) {
		rtn = recv(fd, p, sizeof(buf) - nll, 0);
		if (rtn<0) {
			err("recv failed: %m");
			return -1;
		}

		if (rtn == 0) {
			err("EOF on netlink\n");
			return -1;
		}

		nlp = (struct nlmsghdr *) p;
		if(nlp->nlmsg_type == NLMSG_DONE)
			return 0;
		if (nlp->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *err = (struct nlmsgerr*)NLMSG_DATA(nlp);
			errno=-err->error;
			if (errno) {
				return -1;
			}
			return 0;
		}
		// increment the buffer pointer to place
		// next message
		p += rtn;
		// increment the total size by the size of
		// the last received message
		nll += rtn;
		if((la.nl_groups & RTMGRP_IPV4_ROUTE)
				== RTMGRP_IPV4_ROUTE)
			break;
	}
	return 0;
}

int read_reply()
{
	//string to hold content of the route
	// table (i.e. one entry)
	char dsts[24], gws[24], ifs[16], ms[24];
	// outer loop: loops thru all the NETLINK
	// headers that also include the route entry
	// header
	nlp = (struct nlmsghdr *) buf;
	for(;NLMSG_OK(nlp, nll);nlp=NLMSG_NEXT(nlp, nll))
	{
		// get route entry header
		rtp = (struct rtmsg *) NLMSG_DATA(nlp);
		// we are only concerned about the
		// main route table
		if(rtp->rtm_table != RT_TABLE_MAIN)
			continue;
		// init all the strings
		bzero(dsts, sizeof(dsts));
		bzero(gws, sizeof(gws));
		bzero(ifs, sizeof(ifs));
		bzero(ms, sizeof(ms));
		// inner loop: loop thru all the attributes of
		// one route entry
		rtap = (struct rtattr *) RTM_RTA(rtp);
		rtl = RTM_PAYLOAD(nlp);
		for(;RTA_OK(rtap, rtl);rtap=RTA_NEXT(rtap,rtl))
		{
			switch(rtap->rta_type)
			{
				// destination IPv4 address
				case RTA_DST:
					inet_ntop(AF_INET, RTA_DATA(rtap),
							dsts, 24);
					break;
					// next hop IPv4 address
				case RTA_GATEWAY:
					inet_ntop(AF_INET, RTA_DATA(rtap),
							gws, 24);
					break;
					// unique ID associated with the network
					// interface
				case RTA_OIF:
					sprintf(ifs, "%d",
							*((int *) RTA_DATA(rtap)));
				default:
					break;
			}
		}
		sprintf(ms, "%d", rtp->rtm_dst_len);
		test_msg("dst %s/%s gw %s if %s\n",
				dsts, ms, gws, ifs);
	}
	return 0;
}

int form_request_add()
{
	// attributes of the route entry
	int ifcn = 1; //interface number
	// initialize RTNETLINK request buffer
	bzero(&req, sizeof(req));
	// compute the initial length of the
	// service request
	rtl = sizeof(struct rtmsg);
	// add first attrib:
	// set destination IP addr and increment the
	// RTNETLINK buffer size
	rtap = (struct rtattr *) req.buf;
	rtap->rta_type = RTA_DST;
	rtap->rta_len = sizeof(struct rtattr) + 4;
	inet_pton(AF_INET, dsts,
			((char *)rtap) + sizeof(struct rtattr));
	rtl += rtap->rta_len;
	// add second attrib:
	// set ifc index and increment the size
	rtap = (struct rtattr *) (((char *)rtap)
			+ rtap->rta_len);
	rtap->rta_type = RTA_OIF;//Output interface index
	rtap->rta_len = sizeof(struct rtattr) + 4;
	memcpy(((char *)rtap) + sizeof(struct rtattr),
			&ifcn, sizeof(int));
	rtl += rtap->rta_len;
	// setup the NETLINK header
	req.nl.nlmsg_len = NLMSG_LENGTH(rtl);
	req.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
	req.nl.nlmsg_type = RTM_NEWROUTE;
	// setup the service header (struct rtmsg)
	req.rt.rtm_family = AF_INET;
	req.rt.rtm_table = RT_TABLE_MAIN;
	req.rt.rtm_protocol = RTPROT_STATIC;
	req.rt.rtm_scope = RT_SCOPE_UNIVERSE;
	req.rt.rtm_type = RTN_UNICAST;
	// set the network prefix size
	req.rt.rtm_dst_len = pn;
	return 0;
}
int form_request_del()
{
	// attributes of the route entry
	// initialize RTNETLINK request buffer
	bzero(&req, sizeof(req));
	// compute the initial length of the
	// service request
	rtl = sizeof(struct rtmsg);
	// add first attrib:
	// set destination IP addr and increment the
	// RTNETLINK buffer size
	rtap = (struct rtattr *) req.buf;
	rtap->rta_type = RTA_DST;
	rtap->rta_len = sizeof(struct rtattr) + 4;
	inet_pton(AF_INET, dsts,
			((char *)rtap) + sizeof(struct rtattr));
	rtl += rtap->rta_len;
	// setup the NETLINK header
	req.nl.nlmsg_len = NLMSG_LENGTH(rtl);
	req.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ACK;
	req.nl.nlmsg_type = RTM_DELROUTE;
	// setup the service header (struct rtmsg)
	req.rt.rtm_family = AF_INET;
	req.rt.rtm_table = RT_TABLE_MAIN;
	req.rt.rtm_protocol = RTPROT_STATIC;
	req.rt.rtm_scope = RT_SCOPE_UNIVERSE;
	req.rt.rtm_type = RTN_UNICAST;
	// set the network prefix size
	req.rt.rtm_dst_len = pn;
	return 0;
}
