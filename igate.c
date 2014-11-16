//
//    This file is part of Dire Wolf, an amateur radio packet TNC.
//
//    Copyright (C) 2013, 2014  John Langner, WB2OSZ
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//


/*------------------------------------------------------------------
 *
 * Module:      igate.c
 *
 * Purpose:   	IGate client.
 *		
 * Description:	Establish connection with a tier 2 IGate server
 *		and relay packets between RF and Internet.
 *
 * References:	APRS-IS (Automatic Packet Reporting System-Internet Service)
 *		http://www.aprs-is.net/Default.aspx
 *
 *		APRS iGate properties
 *		http://wiki.ham.fi/APRS_iGate_properties
 *
 *---------------------------------------------------------------*/

/*------------------------------------------------------------------
 *
 * From http://windows.microsoft.com/en-us/windows7/ipv6-frequently-asked-questions
 * 
 * How can I enable IPv6?
 * Follow these steps:
 * 
 * Open Network Connections by clicking the Start button, and then clicking 
 * Control Panel. In the search box, type adapter, and then, under Network 
 * and Sharing Center, click View network connections.
 * 
 * Right-click your network connection, and then click Properties.   
 * If you're prompted for an administrator password or confirmation, type 
 * the password or provide confirmation.
 *
 * Select the check box next to Internet Protocol Version 6 (TCP/IPv6).
 *
 *---------------------------------------------------------------*/

/*
 * Native Windows:	Use the Winsock interface.
 * Linux:		Use the BSD socket interface.
 * Cygwin:		Can use either one.
 */


#if __WIN32__

/* The goal is to support Windows XP and later. */

#include <winsock2.h>
// default is 0x0400
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501	/* Minimum OS version is XP. */
//#define _WIN32_WINNT 0x0502	/* Minimum OS version is XP with SP2. */
//#define _WIN32_WINNT 0x0600	/* Minimum OS version is Vista. */
#include <ws2tcpip.h>
#else 
#include <stdlib.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#endif

#include <unistd.h>
#include <stdio.h>
#include <assert.h>

#include <string.h>


#include "direwolf.h"
#include "ax25_pad.h"
#include "textcolor.h"
#include "version.h"
#include "digipeater.h"
#include "tq.h"
#include "igate.h"
#include "latlong.h"



#if __WIN32__
static unsigned __stdcall connnect_thread (void *arg);
static unsigned __stdcall igate_recv_thread (void *arg);
#else
static void * connnect_thread (void *arg);
static void * igate_recv_thread (void *arg);
#endif

static void send_msg_to_server (char *msg);
static void xmit_packet (char *message);

static void rx_to_ig_init (void);
static void rx_to_ig_remember (packet_t pp);
static int rx_to_ig_allow (packet_t pp);

static void ig_to_tx_init (void);
static void ig_to_tx_remember (packet_t pp);
static int ig_to_tx_allow (packet_t pp);


/* 
 * File descriptor for socket to IGate server. 
 * Set to -1 if not connected. 
 * (Don't use SOCKET type because it is unsigned.) 
*/

static volatile int igate_sock = -1;	

/*
 * After connecting to server, we want to make sure
 * that the login sequence is sent first.
 * This is set to true after the login is complete.
 */

static volatile int ok_to_send = 0;




/*
 * Convert Internet address to text.
 * Can't use InetNtop because it is supported only on Windows Vista and later. 
 */

static char * ia_to_text (int  Family, void * pAddr, char * pStringBuf, size_t StringBufSize)
{
	struct sockaddr_in *sa4;
	struct sockaddr_in6 *sa6;

	switch (Family) {
	  case AF_INET:
	    sa4 = (struct sockaddr_in *)pAddr;
#if __WIN32__
	    sprintf (pStringBuf, "%d.%d.%d.%d", sa4->sin_addr.S_un.S_un_b.s_b1,
						sa4->sin_addr.S_un.S_un_b.s_b2,
						sa4->sin_addr.S_un.S_un_b.s_b3,
						sa4->sin_addr.S_un.S_un_b.s_b4);
#else
	    inet_ntop (AF_INET, &(sa4->sin_addr), pStringBuf, StringBufSize);
#endif
	    break;
	  case AF_INET6:
	    sa6 = (struct sockaddr_in6 *)pAddr;
#if __WIN32__
	    sprintf (pStringBuf, "%x:%x:%x:%x:%x:%x:%x:%x",  
					ntohs(((unsigned short *)(&(sa6->sin6_addr)))[0]),
					ntohs(((unsigned short *)(&(sa6->sin6_addr)))[1]),
					ntohs(((unsigned short *)(&(sa6->sin6_addr)))[2]),
					ntohs(((unsigned short *)(&(sa6->sin6_addr)))[3]),
					ntohs(((unsigned short *)(&(sa6->sin6_addr)))[4]),
					ntohs(((unsigned short *)(&(sa6->sin6_addr)))[5]),
					ntohs(((unsigned short *)(&(sa6->sin6_addr)))[6]),
					ntohs(((unsigned short *)(&(sa6->sin6_addr)))[7]));
#else
	    inet_ntop (AF_INET6, &(sa6->sin6_addr), pStringBuf, StringBufSize);
#endif
	    break;
	  default:
	    sprintf (pStringBuf, "Invalid address family!");
	}
	assert (strlen(pStringBuf) < StringBufSize);
	return pStringBuf;
}


#if ITEST

/* For unit testing. */

int main (int argc, char *argv[])
{
	struct igate_config_s igate_config;
	struct digi_config_s digi_config;
	packet_t pp;

	memset (&igate_config, 0, sizeof(igate_config));

	strcpy (igate_config.t2_server_name, "localhost");
	igate_config.t2_server_port = 14580;
	strcpy (igate_config.t2_login, "WB2OSZ-JL");
	strcpy (igate_config.t2_passcode, "-1");
	igate_config.t2_filter = strdup ("r/1/2/3");
	
	igate_config.tx_chan = 0;
	strcpy (igate_config.tx_via, ",WIDE2-1");
	igate_config.tx_limit_1 = 3;
	igate_config.tx_limit_5 = 5;

	memset (&digi_config, 0, sizeof(digi_config));
	digi_config.num_chans = 2;
	strcpy (digi_config.mycall[0], "WB2OSZ-1");
	strcpy (digi_config.mycall[1], "WB2OSZ-2");

	igate_init(&igate_config, &digi_config);

	while (igate_sock == -1) {
	  SLEEP_SEC(1);
	}

	SLEEP_SEC (2);
	pp = ax25_from_text ("A>B,C,D:Ztest message 1", 0);
	igate_send_rec_packet (0, pp);
	ax25_delete (pp);

	SLEEP_SEC (2);
	pp = ax25_from_text ("A>B,C,D:Ztest message 2", 0);
	igate_send_rec_packet (0, pp);
	ax25_delete (pp);

	SLEEP_SEC (2);
	pp = ax25_from_text ("A>B,C,D:Ztest message 2", 0);   /* Should suppress duplicate. */
	igate_send_rec_packet (0, pp);
	ax25_delete (pp);

	SLEEP_SEC (2);
	pp = ax25_from_text ("A>B,TCPIP,D:ZShould drop this due to path", 0);
	igate_send_rec_packet (0, pp);
	ax25_delete (pp);

	SLEEP_SEC (2);
	pp = ax25_from_text ("A>B,C,D:?Should drop query", 0);
	igate_send_rec_packet (0, pp);
	ax25_delete (pp);

	SLEEP_SEC (5);
	pp = ax25_from_text ("A>B,C,D:}E>F,G*,H:Zthird party stuff", 0);
	igate_send_rec_packet (0, pp);
	ax25_delete (pp);

#if 1
	while (1) {
	  SLEEP_SEC (20);
	  text_color_set(DW_COLOR_INFO);
	  dw_printf ("Send received packet\n");
	  send_msg_to_server ("W1ABC>APRS:?\r\n");
	}
#endif
	return 0;
}

#endif


/*
 * Global stuff (to this file)
 *
 * These are set by init function and need to 
 * be kept around in case connection is lost and
 * we need to reestablish the connection later.
 */

static struct igate_config_s g_config;

static int g_num_chans;			/* Number of radio channels. */

static char g_mycall[MAX_CHANS][AX25_MAX_ADDR_LEN];
					/* Call-ssid associated */
                                        /* with each of the radio channels.  */
                                        /* Could be the same or different. */


/*
 * Statistics.  
 * TODO: need print function.
 */

static int stats_failed_connect;	/* Number of times we tried to connect to */
					/* a server and failed.  A small number is not */
					/* a bad thing.  Each name should have a bunch */
					/* of addresses for load balancing and */
					/* redundancy. */

static int stats_connects;		/* Number of successful connects to a server. */
					/* Normally you'd expect this to be 1.  */
					/* Could be larger if one disappears and we */
					/* try again to find a different one. */

static time_t stats_connect_at;		/* Most recent time connection was established. */
					/* can be used to determine elapsed connect time. */

static int stats_rf_recv_packets;	/* Number of candidate packets from the radio. */

static int stats_rx_igate_packets;	/* Number of packets passed along to the IGate */
					/* server after filtering. */

static int stats_uplink_bytes;		/* Total number of bytes sent to IGate server */
					/* including login, packets, and hearbeats. */

static int stats_downlink_bytes;	/* Total number of bytes from IGate server including */
					/* packets, heartbeats, other messages. */

static int stats_tx_igate_packets;	/* Number of packets from IGate server. */

static int stats_rf_xmit_packets;	/* Number of packets passed along to radio */
					/* after rate limiting or other restrictions. */



/*-------------------------------------------------------------------
 *
 * Name:        igate_init
 *
 * Purpose:     One time initialization when main application starts up.
 *
 * Inputs:	p_igate_config	- IGate configuration.
 *
 *		p_digi_config	- Digipeater configuration.  All we care about is:
 *				  - Number of radio channels.
 *				  - Radio call and SSID for each channel.
 *
 * Description:	This starts two threads:
 *
 *		  *  to establish and maintain a connection to the server.
 *		  *  to listen for packets from the server.
 *
 *--------------------------------------------------------------------*/


void igate_init (struct igate_config_s *p_igate_config, struct digi_config_s *p_digi_config)
{
#if __WIN32__
	HANDLE connnect_th;
	HANDLE cmd_recv_th;
#else
	pthread_t connect_listen_tid;
	pthread_t cmd_listen_tid;
	int e;
#endif
	int j;

#if DEBUGx
	text_color_set(DW_COLOR_DEBUG);
	dw_printf ("igate_init ( %s, %d, %s, %s, %s )\n", 
				p_igate_config->t2_server_name, 
				p_igate_config->t2_server_port, 
				p_igate_config->t2_login, 
				p_igate_config->t2_passcode, 
				p_igate_config->t2_filter);
#endif


	stats_failed_connect = 0;	
	stats_connects = 0;		
	stats_connect_at = 0;		
	stats_rf_recv_packets = 0;	
	stats_rx_igate_packets = 0;	
	stats_uplink_bytes = 0;		
	stats_downlink_bytes = 0;	
	stats_tx_igate_packets = 0;	
	stats_rf_xmit_packets = 0;
	
	rx_to_ig_init ();
	ig_to_tx_init ();
/*
 * Save the arguments for later use.
 */
	memcpy (&g_config, p_igate_config, sizeof (g_config));

	g_num_chans = p_digi_config->num_chans;
	assert (g_num_chans >= 1 && g_num_chans <= MAX_CHANS);
	for (j=0; j<g_num_chans; j++) {
	  strcpy (g_mycall[j], p_digi_config->mycall[j]);
	}


/*
 * Continue only if we have server name, login, and passcode.
 */
	if (strlen(p_igate_config->t2_server_name) == 0 ||
	    strlen(p_igate_config->t2_login) == 0 ||
	    strlen(p_igate_config->t2_passcode) == 0) {
	  return;
	}

/*
 * This connects to the server and sets igate_sock.
 * It also sends periodic messages to say I'm still here.
 */

#if __WIN32__
	connnect_th = (HANDLE)_beginthreadex (NULL, 0, connnect_thread, (void *)NULL, 0, NULL);
	if (connnect_th == NULL) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Internal error: Could not create IGate connection thread\n");
	  return;
	}
#else
	e = pthread_create (&connect_listen_tid, NULL, connnect_thread, (void *)NULL);
	if (e != 0) {
	  text_color_set(DW_COLOR_ERROR);
	  perror("Internal error: Could not create IGate connection thread");
	  return;
	}
#endif

/*
 * This reads messages from client when igate_sock is valid.
 */

#if __WIN32__
	cmd_recv_th = (HANDLE)_beginthreadex (NULL, 0, igate_recv_thread, NULL, 0, NULL);
	if (cmd_recv_th == NULL) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Internal error: Could not create IGate reading thread\n");
	  return;
	}
#else
	e = pthread_create (&cmd_listen_tid, NULL, igate_recv_thread, NULL);
	if (e != 0) {
	  text_color_set(DW_COLOR_ERROR);
	  perror("Internal error: Could not create IGate reading thread");
	  return;
	}
#endif
}


/*-------------------------------------------------------------------
 *
 * Name:        connnect_thread
 *
 * Purpose:     Establish connection with IGate server.
 *		Send periodic heartbeat to keep keep connection active.
 *		Reconnect if something goes wrong and we got disconnected.
 *
 * Inputs:	arg		- Not used.
 *
 * Outputs:	igate_sock	- File descriptor for communicating with client app.
 *				  Will be -1 if not connected.
 *
 * References:	TCP client example.
 *		http://msdn.microsoft.com/en-us/library/windows/desktop/ms737591(v=vs.85).aspx
 *
 *		Linux IPv6 HOWTO
 *		http://www.tldp.org/HOWTO/Linux+IPv6-HOWTO/
 *
 *--------------------------------------------------------------------*/

/*
 * Addresses don't get mixed up very well.
 * IPv6 always shows up last so we'd probably never
 * end up using any of them.  Use our own shuffle.
 */

static void shuffle (struct addrinfo *host[], int nhosts)
{
        int j, k;

        assert (RAND_MAX >= nhosts);  /* for % to work right */

        if (nhosts < 2) return;

        srand (time(NULL));

        for (j=0; j<nhosts; j++) {
          k = rand() % nhosts;
          assert (k >=0 && k<nhosts);
          if (j != k) {
            struct addrinfo *temp;
            temp = host[j]; host[j] = host[k]; host[k] = temp;
          }
        }
}

#define MAX_HOSTS 50




#if __WIN32__
static unsigned __stdcall connnect_thread (void *arg)
#else
static void * connnect_thread (void *arg)	
#endif	
{
	struct addrinfo hints;
	struct addrinfo *ai_head = NULL;
	struct addrinfo *ai;
	struct addrinfo *hosts[MAX_HOSTS];
	int num_hosts, n;
	int err;
	char server_port_str[12];	/* text form of port number */
	char ipaddr_str[46];		/* text form of IP address */
#if __WIN32__
	WSADATA wsadata;
#endif

	sprintf (server_port_str, "%d", g_config.t2_server_port);
#if DEBUGx
	text_color_set(DW_COLOR_DEBUG);
        dw_printf ("DEBUG: igate connect_thread start, port = %d = '%s'\n", g_config.t2_server_port, server_port_str);
#endif

#if __WIN32__
	err = WSAStartup (MAKEWORD(2,2), &wsadata);
	if (err != 0) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf("WSAStartup failed: %d\n", err);
	    return (0);
	}

	if (LOBYTE(wsadata.wVersion) != 2 || HIBYTE(wsadata.wVersion) != 2) {
	  text_color_set(DW_COLOR_ERROR);
          dw_printf("Could not find a usable version of Winsock.dll\n");
          WSACleanup();
	  //sleep (1);
          return (0);
	}
#endif

	memset (&hints, 0, sizeof(hints));

	hints.ai_family = AF_UNSPEC;	/* Allow either IPv4 or IPv6. */

	// IPv6 is half baked on Windows XP.
	// We might need to leave out IPv6 support for Windows version.
	// hints.ai_family = AF_INET;	/* IPv4 only. */

#if IPV6_ONLY
	/* IPv6 addresses always show up at end of list. */
	/* Force use of them for testing. */
	hints.ai_family = AF_INET6;	/* IPv6 only */
#endif					
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;


/*
 * Repeat forever.
 */

	while (1) {

/*
 * Connect to IGate server if not currently connected.
 */

	  if (igate_sock == -1) {

	    SLEEP_SEC (5);

	    ai_head = NULL;
	    err = getaddrinfo(g_config.t2_server_name, server_port_str, &hints, &ai_head);
	    if (err != 0) {
	      text_color_set(DW_COLOR_ERROR);
#if __WIN32__
	      dw_printf ("Can't get address for IGate server %s, err=%d\n", 
					g_config.t2_server_name, WSAGetLastError());
#else 
	      dw_printf ("Can't get address for IGate server %s, %s\n", 
					g_config.t2_server_name, gai_strerror(err));
#endif
	      freeaddrinfo(ai_head);

	      continue;
	    }

#if DEBUG_DNS
	    text_color_set(DW_COLOR_DEBUG);
	    dw_printf ("getaddrinfo returns:\n");
#endif
	    num_hosts = 0;
	    for (ai = ai_head; ai != NULL; ai = ai->ai_next) {
#if DEBUG_DNS
	      text_color_set(DW_COLOR_DEBUG);
	      ia_to_text (ai->ai_family, ai->ai_addr, ipaddr_str, sizeof(ipaddr_str));
	      dw_printf ("    %s\n", ipaddr_str);
#endif
	      hosts[num_hosts] = ai;
	      if (num_hosts < MAX_HOSTS) num_hosts++;
	    }

	    // We can get multiple addresses back for the host name.
	    // These should be somewhat randomized for load balancing. 
	    // It turns out the IPv6 addresses are always at the 
	    // end for both Windows and Linux.   We do our own shuffling
	    // to mix them up better and give IPv6 a chance. 

	    shuffle (hosts, num_hosts);

#if DEBUG_DNS
	    text_color_set(DW_COLOR_DEBUG);
	    dw_printf ("after shuffling:\n");
	    for (n=0; n<num_hosts; n++) {
	      ia_to_text (hosts[n]->ai_family, hosts[n]->ai_addr, ipaddr_str, sizeof(ipaddr_str));
	      dw_printf ("    %s\n", ipaddr_str);
	    }
#endif

	    // Try each address until we find one that is successful.

	    for (n=0; n<num_hosts; n++) {
	      int is;

	      ai = hosts[n];

	      ia_to_text (ai->ai_family, ai->ai_addr, ipaddr_str, sizeof(ipaddr_str));
	      is = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#if __WIN32__
	      if (is == INVALID_SOCKET) {
	        text_color_set(DW_COLOR_ERROR);
	        dw_printf ("IGate: Socket creation failed, err=%d", WSAGetLastError());
	        WSACleanup();
	        is = -1;
		stats_failed_connect++;
	        continue;
	      }
#else
	      if (err != 0) {
	        text_color_set(DW_COLOR_INFO);
	        dw_printf("Connect to IGate server %s (%s) failed.\n\n",
					g_config.t2_server_name, ipaddr_str);
	        (void) close (is);
	        is = -1;
		stats_failed_connect++;
	        continue;
	      }
#endif

#ifndef DEBUG_DNS 
	      err = connect(is, ai->ai_addr, (int)ai->ai_addrlen);
#if __WIN32__
	      if (err == SOCKET_ERROR) {
	        text_color_set(DW_COLOR_INFO);
	        dw_printf("Connect to IGate server %s (%s) failed.\n\n",
					g_config.t2_server_name, ipaddr_str);
	        closesocket (is);
	        is = -1;
		stats_failed_connect++; 
	        continue;
	      }
	      // TODO: set TCP_NODELAY?
#else
	      if (err != 0) {
	        text_color_set(DW_COLOR_INFO);
	        dw_printf("Connect to IGate server %s (%s) failed.\n\n",
					g_config.t2_server_name, ipaddr_str);
	        (void) close (is);
	        is = -1;
		stats_failed_connect++;
	        continue;
	      }
	      /* IGate documentation says to use it.  */
	      /* Does it really make a difference for this application? */
	      int flag = 1;
	      err = setsockopt (is, IPPROTO_TCP, TCP_NODELAY, (void*)(long)(&flag), sizeof(flag));
	      if (err < 0) {
	        text_color_set(DW_COLOR_INFO);
	        dw_printf("setsockopt TCP_NODELAY failed.\n");
	      }
#endif
	      stats_connects++;
	      stats_connect_at = time(NULL);

/* Success. */

	      text_color_set(DW_COLOR_INFO);
 	      dw_printf("\nNow connected to IGate server %s (%s)\n", g_config.t2_server_name, ipaddr_str );
	      if (strchr(ipaddr_str, ':') != NULL) {
	      	dw_printf("Check server status here http://[%s]:14501\n\n", ipaddr_str);
	      }
	      else {
	        dw_printf("Check server status here http://%s:14501\n\n", ipaddr_str);
	      }

/* 
 * Set igate_sock so everyone else can start using it. 
 * But make the Rx -> Internet messages wait until after login.
 */

	      ok_to_send = 0;
	      igate_sock = is;
#endif	  
	      break;
	    }

	    freeaddrinfo(ai_head);

	    if (igate_sock != -1) {
	      char stemp[256];

/* 
 * Send login message.
 * Software name and version must not contain spaces.
 */

	      SLEEP_SEC(3);
	      sprintf (stemp, "user %s pass %s vers Dire-Wolf %d.%d", 
			g_config.t2_login, g_config.t2_passcode,
			MAJOR_VERSION, MINOR_VERSION);
	      if (g_config.t2_filter != NULL) {
	        strcat (stemp, " filter ");
	        strcat (stemp, g_config.t2_filter);
	      }
	      strcat (stemp, "\r\n");
	      send_msg_to_server (stemp);

/* Delay until it is ok to start sending packets. */

	      SLEEP_SEC(7);
	      ok_to_send = 1;
	    }
	  }

/*
 * If connected to IGate server, send heartbeat periodically to keep connection active.
 */
	  if (igate_sock != -1) {
	    SLEEP_SEC(10);
	  }
	  if (igate_sock != -1) {
	    SLEEP_SEC(10);
	  }
	  if (igate_sock != -1) {
	    SLEEP_SEC(10);
	  }


	  if (igate_sock != -1) {

	    char heartbeat[10];

	    strcpy (heartbeat, "#\r\n");

	    /* This will close the socket if any error. */
	    send_msg_to_server (heartbeat);

	  }
	}
} /* end connnect_thread */




/*-------------------------------------------------------------------
 *
 * Name:        igate_send_rec_packet
 *
 * Purpose:     Send a packet to the IGate server
 *
 * Inputs:	chan	- Radio channel it was received on.
 *
 *		recv_pp	- Pointer to packet object.
 *			  *** CALLER IS RESPONSIBLE FOR DELETING IT! **
 *		
 *
 * Description:	Send message to IGate Server if connected.
 *
 * Assumptions:	(1) Caller has already verified it is an APRS packet.
 *		i.e. control = 3 for UI frame, protocol id = 0xf0 for no layer 3
 *
 *		(2) This is being called only for packets received with
 *		a correct CRC.  We don't want to propagate corrupted data.
 *
 *--------------------------------------------------------------------*/

void igate_send_rec_packet (int chan, packet_t recv_pp)
{
	packet_t pp;
	int n;
	unsigned char *pinfo;
	char *p;
	char msg[520];		/* Message to IGate max 512 characters. */
	int info_len;
	

	if (igate_sock == -1) {
	  return;	/* Silently discard if not connected. */
	}

	if ( ! ok_to_send) {
	  return;	/* Login not complete. */
	}

	/* Count only while connected. */
	stats_rf_recv_packets++;

/*
 * First make a copy of it because it might be modified in place.
 */

	pp = ax25_dup (recv_pp);

/*
 * Third party frames require special handling to unwrap payload.
 */
	while (ax25_get_dti(pp) == '}') {
	  packet_t inner_pp;

	  for (n = 0; n < ax25_get_num_repeaters(pp); n++) {
	    char via[AX25_MAX_ADDR_LEN];	/* includes ssid. Do we want to ignore it? */

	    ax25_get_addr_with_ssid (pp, n + AX25_REPEATER_1, via);

	    if (strcmp(via, "TCPIP") == 0 ||
	        strcmp(via, "TCPXX") == 0 ||
	        strcmp(via, "RFONLY") == 0 ||
	        strcmp(via, "NOGATE") == 0) {
#if DEBUGx
	      text_color_set(DW_COLOR_DEBUG);
	      dw_printf ("Rx IGate: Do not relay with TCPIP etc. in path.\n");
#endif
	      ax25_delete (pp);
	      return;
	    }
	  }

#if DEBUGx
	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("Rx IGate: Unwrap third party message.\n");
#endif
	  inner_pp = ax25_unwrap_third_party(pp);
	  if (inner_pp == NULL) {
	    ax25_delete (pp);
	    return;
	  }
	  ax25_delete (pp);
	  pp = inner_pp;
	}

/* 
 * Do not relay packets with TCPIP, TCPXX, RFONLY, or NOGATE in the via path.
 */
	for (n = 0; n < ax25_get_num_repeaters(pp); n++) {
	  char via[AX25_MAX_ADDR_LEN];	/* includes ssid. Do we want to ignore it? */

	  ax25_get_addr_with_ssid (pp, n + AX25_REPEATER_1, via);

	  if (strcmp(via, "TCPIP") == 0 ||
	      strcmp(via, "TCPXX") == 0 ||
	      strcmp(via, "RFONLY") == 0 ||
	      strcmp(via, "NOGATE") == 0) {
#if DEBUGx
	    text_color_set(DW_COLOR_DEBUG);
	    dw_printf ("Rx IGate: Do not relay with TCPIP etc. in path.\n");
#endif
	    ax25_delete (pp);
	    return;
	  }
	}

/*
 * Do not relay generic query.
 */
	if (ax25_get_dti(pp) == '?') {
#if DEBUGx
	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("Rx IGate: Do not relay generic query.\n");
#endif
	  ax25_delete (pp);
	  return;
	}


/*
 * Cut the information part at the first CR or LF.
 */

	info_len = ax25_get_info (pp, &pinfo);

	if ((p = strchr ((char*)pinfo, '\r')) != NULL) {
#if DEBUGx
	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("Rx IGate: Truncated information part at CR.\n");
#endif
          *p = '\0';
	}

	if ((p = strchr ((char*)pinfo, '\n')) != NULL) {
#if DEBUGx
	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("Rx IGate: Truncated information part at LF.\n");
#endif
          *p = '\0';
	}


/*
 * Someone around here occasionally sends a packet with no information part.
 */
	if (strlen(pinfo) == 0) {

#if DEBUGx
	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("Rx IGate: Information part length is zero.\n");
#endif
	  ax25_delete (pp);
	  return;
	}

// TODO: Should we drop raw touch tone data object type generated here?

/*
 * Do not relay if a duplicate of something sent recently.
 */

	if ( ! rx_to_ig_allow(pp)) {
#if DEBUG
	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("Rx IGate: Drop duplicate of same packet seen recently.\n");
#endif
	  ax25_delete (pp);
	  return;
	}

/* 
 * Finally, append ",qAR," and my call to the path.
 */

	ax25_format_addrs (pp, msg);
	msg[strlen(msg)-1] = '\0';    /* Remove trailing ":" */
	strcat (msg, ",qAR,");
	strcat (msg, g_mycall[chan]);
	strcat (msg, ":");
	strcat (msg, (char*)pinfo);
	strcat (msg, "\r\n");

	send_msg_to_server (msg);
	stats_rx_igate_packets++;

/*
 * Remember what was sent to avoid duplicates in near future.
 */
	rx_to_ig_remember (pp);

	ax25_delete (pp);

} /* end igate_send_rec_packet */





/*-------------------------------------------------------------------
 *
 * Name:        send_msg_to_server
 *
 * Purpose:     Send to the IGate server.
 *		This one function should be used for login, hearbeats,
 *		and packets.
 *
 * Inputs:	msg	- Message.  Should end with CR/LF.
 *		
 *
 * Description:	Send message to IGate Server if connected.
 *		Disconnect from server, and notify user, if any error.
 *
 *--------------------------------------------------------------------*/


static void send_msg_to_server (char *msg)
{
	int err;


	if (igate_sock == -1) {
	  return;	/* Silently discard if not connected. */
	}

	stats_uplink_bytes += strlen(msg);

#if DEBUG
	text_color_set(DW_COLOR_XMIT);
	dw_printf ("[ig] ");
	ax25_safe_print (msg, strlen(msg), 0);
	dw_printf ("\n");
#endif

#if __WIN32__	
        err = send (igate_sock, msg, strlen(msg), 0);
	if (err == SOCKET_ERROR)
	{
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("\nError %d sending message to IGate server.  Closing connection.\n\n", WSAGetLastError());
	  //dw_printf ("DEBUG: igate_sock=%d, line=%d\n", igate_sock, __LINE__);
	  closesocket (igate_sock);
	  igate_sock = -1;
	  WSACleanup();
	}
#else
        err = write (igate_sock, msg, strlen(msg));
	if (err <= 0)
	{
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("\nError sending message to IGate server.  Closing connection.\n\n");
	  close (igate_sock);
	  igate_sock = -1;    
	}
#endif
	
} /* end send_msg_to_server */


/*-------------------------------------------------------------------
 *
 * Name:        get1ch
 *
 * Purpose:     Read one byte from socket.
 *
 * Inputs:	igate_sock	- file handle for socket.
 *
 * Returns:	One byte from stream.
 *		Waits and tries again later if any error.
 *
 *
 *--------------------------------------------------------------------*/

static int get1ch (void)
{
	unsigned char ch;
	int n;

	while (1) {

	  while (igate_sock == -1) {
	    SLEEP_SEC(5);			/* Not connected.  Try again later. */
	  }

	  /* Just get one byte at a time. */
	  // TODO: might read complete packets and unpack from own buffer
	  // rather than using a system call for each byte.

#if __WIN32__
	  n = recv (igate_sock, (char*)(&ch), 1, 0);
#else
	  n = read (igate_sock, &ch, 1);
#endif

	  if (n == 1) {
#if DEBUG9
	    dw_printf (log_fp, "%02x %c %c", ch, 
			isprint(ch) ? ch : '.' , 
			(isupper(ch>>1) || isdigit(ch>>1) || (ch>>1) == ' ') ? (ch>>1) : '.');
	    if (ch == '\r') fprintf (log_fp, "  CR");
	    if (ch == '\n') fprintf (log_fp, "  LF");
	    fprintf (log_fp, "\n");
#endif
	    return(ch);	
	  }

          text_color_set(DW_COLOR_ERROR);
	  dw_printf ("\nError reading from IGate server.  Closing connection.\n\n");
#if __WIN32__
	  closesocket (igate_sock);
#else
	  close (igate_sock);
#endif
	  igate_sock = -1;
	}

} /* end get1ch */




/*-------------------------------------------------------------------
 *
 * Name:        igate_recv_thread
 *
 * Purpose:     Wait for messages from IGate Server.
 *
 * Inputs:	arg		- Not used.
 *
 * Outputs:	igate_sock	- File descriptor for communicating with client app.
 *
 * Description:	Process messages from the IGate server.
 *
 *--------------------------------------------------------------------*/

#if __WIN32__
static unsigned __stdcall igate_recv_thread (void *arg)
#else
static void * igate_recv_thread (void *arg)
#endif
{
	unsigned char ch;
	unsigned char message[1000];  // Spec says max 500 or so.
	int len;
	
			
#if DEBUGx
	text_color_set(DW_COLOR_DEBUG);
	dw_printf ("igate_recv_thread ( socket = %d )\n", igate_sock);
#endif

	while (1) {

	  len = 0;

	  do
	  {
	    ch = get1ch();
	    stats_downlink_bytes++;

	    if (len < sizeof(message)) 
	    {
	      message[len] = ch;
	    }
	    len++;
	    
	  } while (ch != '\n');

/*
 * We have a complete message terminated by LF.
 */
	  if (len == 0) 
	  {
/* 
 * Discard if zero length. 
 */
	  }
	  else if (message[0] == '#') {
/*
 * Heartbeat or other control message.
 *
 * Print only if within seconds of logging in.
 * That way we can see login confirmation but not 
 * be bothered by the heart beat messages.
 */
#ifndef DEBUG
	    if ( ! ok_to_send) {
#endif
	      text_color_set(DW_COLOR_REC);
	      dw_printf ("[ig] ");
	      ax25_safe_print ((char *)message, len, 0);
	      dw_printf ("\n");
#ifndef DEBUG
	    }
#endif
	  }
	  else 
	  {
/*
 * Convert to third party packet and transmit.
 */
	    text_color_set(DW_COLOR_REC);
	    dw_printf ("\n[ig] ");
	    ax25_safe_print ((char *)message, len, 0);
	    dw_printf ("\n");

/*
 * Remove CR LF from end.
 */
	    if (len >=2 && message[len-1] == '\n') { message[len-1] = '\0'; len--; }
	    if (len >=1 && message[len-1] == '\r') { message[len-1] = '\0'; len--; }

	    xmit_packet ((char*)message);
	  }

	}  /* while (1) */
	return (0);

} /* end igate_recv_thread */


/*-------------------------------------------------------------------
 *
 * Name:        xmit_packet
 *
 * Purpose:     Convert text string, from IGate server, to third party
 *		packet and send to transmit queue.
 *
 * Inputs:	message		- As sent by the server.  
 *
 *--------------------------------------------------------------------*/

static void xmit_packet (char *message)
{
	packet_t pp3;
	char payload[500];	/* what is max len? */
	char *pinfo = NULL;
	int info_len;

/*
 * Is IGate to Radio direction enabled?
 */
	if (g_config.tx_chan == -1) {
	  return;
	}

	stats_tx_igate_packets++;

	assert (g_config.tx_chan >= 0 && g_config.tx_chan < MAX_CHANS);

/*
 * Try to parse it into a packet object.
 * Bug:  Up to 8 digipeaters are allowed in radio format.
 * There is a potential of finding more here.
 */
	pp3 = ax25_from_text(message, 0);
	if (pp3 == NULL) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Tx IGate: Could not parse message from server.\n");
	  dw_printf ("%s\n", message);
	  return;
	}

/*
 * TODO: Discard if qAX in path???  others?
 */

/*
 * Remove the VIA path.
 */
	while (ax25_get_num_repeaters(pp3) > 0) {
	  ax25_remove_addr (pp3, AX25_REPEATER_1);
	}

/* 
 * Replace the VIA path with TCPIP and my call.
 * Mark my call as having been used.
 */
	ax25_set_addr (pp3, AX25_REPEATER_1, "TCPIP");
	ax25_set_h (pp3, AX25_REPEATER_1);
	ax25_set_addr (pp3, AX25_REPEATER_2, g_mycall[g_config.tx_chan]); 
	ax25_set_h (pp3, AX25_REPEATER_2);

/*
 * Convert to text representation.
 */
	ax25_format_addrs (pp3, payload);
	info_len = ax25_get_info (pp3, (unsigned char **)(&pinfo));
	strcat (payload, pinfo);
#if DEBUGx
	text_color_set(DW_COLOR_DEBUG);
	dw_printf ("Tx IGate: payload=%s\n", payload);
#endif
	
/*
 * Encapsulate for sending over radio if no reason to drop it.
 */
	if (ig_to_tx_allow (pp3)) {
	  char radio [500];
	  packet_t pradio;

	  sprintf (radio, "%s>%s%d%d%s:}%s",
				g_mycall[g_config.tx_chan],
				APP_TOCALL, MAJOR_VERSION, MINOR_VERSION,
				g_config.tx_via,
				payload);

	  pradio = ax25_from_text (radio, 1);
#if ITEST
	  text_color_set(DW_COLOR_XMIT);
	  dw_printf ("Xmit: %s\n", radio);
	  ax25_delete (pradio);
#else
	  /* This consumes packet so don't reference it again! */
	  tq_append (g_config.tx_chan, TQ_PRIO_1_LO, pradio);
#endif
	  stats_rf_xmit_packets++;
	  ig_to_tx_remember (pp3);
	}

	ax25_delete (pp3);

} /* end xmit_packet */



/*-------------------------------------------------------------------
 *
 * Name:        rx_to_ig_remember
 *
 * Purpose:     Keep a record of packets sent to the IGate server
 *		so we don't send duplicates within some set amount of time.
 *
 * Inputs:	pp	- Pointer to a packet object.
 *
 *-------------------------------------------------------------------
 *
 * Name:	rx_to_ig_allow
 * 
 * Purpose:	Check whether this is a duplicate of another sent recently.
 *
 * Input:	pp	- Pointer to packet object.
 *		
 * Returns:	True if it is OK to send.
 *		
 *-------------------------------------------------------------------
 *
 * Description: These two functions perform the final stage of filtering
 *		before sending a received (from radio) packet to the IGate server.
 *
 *		rx_to_ig_remember must be called for every packet sent to the server.
 *
 *		rx_to_ig_allow decides whether this should be allowed thru
 *		based on recent activity.  We will drop the packet if it is a
 *		duplicate of another sent recently.
 *
 *		Rather than storing the entire packet, we just keep a CRC to 
 *		reduce memory and processing requirements.  We do the same in
 *		the digipeater function to suppress duplicates.
 *
 *		There is a 1 / 65536 chance of getting a false positive match
 *		which is good enough for this application.
 *
 *--------------------------------------------------------------------*/

#define RX2IG_DEDUPE_TIME 60		/* Do not send duplicate within 60 seconds. */
#define RX2IG_HISTORY_MAX 30		/* Remember the last 30 sent to IGate server. */

static int rx2ig_insert_next;
static time_t rx2ig_time_stamp[RX2IG_HISTORY_MAX];
static unsigned short rx2ig_checksum[RX2IG_HISTORY_MAX];

static void rx_to_ig_init (void)
{
	int n;
	for (n=0; n<RX2IG_HISTORY_MAX; n++) {
	  rx2ig_time_stamp[n] = 0;
	  rx2ig_checksum[n] = 0;
	}
	rx2ig_insert_next = 0;
}
	

static void rx_to_ig_remember (packet_t pp)
{
       	rx2ig_time_stamp[rx2ig_insert_next] = time(NULL);
        rx2ig_checksum[rx2ig_insert_next] = ax25_dedupe_crc(pp);

        rx2ig_insert_next++;
        if (rx2ig_insert_next >= RX2IG_HISTORY_MAX) {
          rx2ig_insert_next = 0;
        }
}

static int rx_to_ig_allow (packet_t pp)
{
	unsigned short crc = ax25_dedupe_crc(pp);
	time_t now = time(NULL);
	int j;

	for (j=0; j<RX2IG_HISTORY_MAX; j++) {
	  if (rx2ig_time_stamp[j] >= now - RX2IG_DEDUPE_TIME && rx2ig_checksum[j] == crc) {
	    return 0;
	  }
	}
	return 1;

} /* end rx_to_ig_allow */



/*-------------------------------------------------------------------
 *
 * Name:        ig_to_tx_remember
 *
 * Purpose:     Keep a record of packets sent from IGate server to radio transmitter
 *		so we don't send duplicates within some set amount of time.
 *
 * Inputs:	pp	- Pointer to a packet object.
 *
 *------------------------------------------------------------------------------
 *
 * Name:	ig_to_tx_allow
 * 
 * Purpose:	Check whether this is a duplicate of another sent recently
 *		or if we exceed the transmit rate limits.
 *
 * Input:	pp	- Pointer to packet object.
 *		
 * Returns:	True if it is OK to send.
 *		
 *------------------------------------------------------------------------------
 *
 * Description: These two functions perform the final stage of filtering
 *		before sending a packet from the IGate server to the radio.
 *
 *		ig_to_tx_remember must be called for every packet, from the IGate 
 *		server, sent to the radio transmitter.
 *
 *		ig_to_tx_allow decides whether this should be allowed thru
 *		based on recent activity.  We will drop the packet if it is a
 *		duplicate of another sent recently.
 *
 *		This is the essentially the same as the pair of functions
 *		above with one addition restriction.  
 *
 *		The typical residential Internet connection is about 10,000
 *		times faster than the radio links we are using.  It would
 *		be easy to completely saturate the radio channel if we are
 *		not careful.
 *
 *		Besides looking for duplicates, this will also tabulate the 
 *		number of packets sent during the past minute and past 5
 *		minutes and stop sending if a limit is reached.
 *
 * Future?	We might also want to avoid transmitting if the same packet
 *		was heard on the radio recently.  If everything is kept in
 *		the same table, we'd need to distinguish between those from
 *		the IGate server and those heard on the radio.
 *		Those heard on the radio would not count toward the
 *		1 and 5 minute rate limiting.
 *		Maybe even provide informative information such as -
 *		Tx IGate: Same packet heard recently from W1ABC and W9XYZ.
 *
 *		Of course, the radio encapsulation would need to be removed
 *		and only the 3rd party packet inside compared.
 *
 *--------------------------------------------------------------------*/

#define IG2TX_DEDUPE_TIME 60		/* Do not send duplicate within 60 seconds. */
#define IG2TX_HISTORY_MAX 50		/* Remember the last 50 sent from server to radio. */

static int ig2tx_insert_next;
static time_t ig2tx_time_stamp[IG2TX_HISTORY_MAX];
static unsigned short ig2tx_checksum[IG2TX_HISTORY_MAX];

static void ig_to_tx_init (void)
{
	int n;
	for (n=0; n<IG2TX_HISTORY_MAX; n++) {
	  ig2tx_time_stamp[n] = 0;
	  ig2tx_checksum[n] = 0;
	}
	ig2tx_insert_next = 0;
}
	

static void ig_to_tx_remember (packet_t pp)
{
       	ig2tx_time_stamp[ig2tx_insert_next] = time(NULL);
        ig2tx_checksum[ig2tx_insert_next] = ax25_dedupe_crc(pp);

        ig2tx_insert_next++;
        if (ig2tx_insert_next >= IG2TX_HISTORY_MAX) {
          ig2tx_insert_next = 0;
        }
}

static int ig_to_tx_allow (packet_t pp)
{
	unsigned short crc = ax25_dedupe_crc(pp);
	time_t now = time(NULL);
	int j;
	int count_1, count_5;

	for (j=0; j<IG2TX_HISTORY_MAX; j++) {
	  if (ig2tx_time_stamp[j] >= now - IG2TX_DEDUPE_TIME && ig2tx_checksum[j] == crc) {
	    text_color_set(DW_COLOR_INFO);
	    dw_printf ("Tx IGate: Drop duplicate packet transmitted recently.\n");
	    return 0;
	  }
	}
	count_1 = 0;
	count_5 = 0;
	for (j=0; j<IG2TX_HISTORY_MAX; j++) {
	  if (ig2tx_time_stamp[j] >= now - 60) count_1++;
	  if (ig2tx_time_stamp[j] >= now - 300) count_5++;
	}

	if (count_1 >= g_config.tx_limit_1) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Tx IGate: Already transmitted maximum of %d packets in 1 minute.\n", g_config.tx_limit_1);
	  return 0;
	}
	if (count_5 >= g_config.tx_limit_5) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Tx IGate: Already transmitted maximum of %d packets in 5 minutes.\n", g_config.tx_limit_5);
	  return 0;
	}

	return 1;

} /* end ig_to_tx_allow */

/* end igate.c */
