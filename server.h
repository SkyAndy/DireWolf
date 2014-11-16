
/* 
 * Name:	server.h
 */


#include "ax25_pad.h"		/* for packet_t */

#include "config.h"


void server_set_debug (int n);

void server_init (struct misc_config_s *misc_config);

void server_send_rec_packet (int chan, packet_t pp, unsigned char *fbuf,  int flen);


/* end server.h */
