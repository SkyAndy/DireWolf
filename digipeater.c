//
//    This file is part of Dire Wolf, an amateur radio packet TNC.
//
//    Copyright (C) 2011,2013  John Langner, WB2OSZ
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
 * Name:	digipeater.c
 *
 * Purpose:	Act as an APRS digital repeater.
 *
 *
 * Description:	Decide whether the specified packet should
 *		be digipeated and make necessary modifications.
 *
 *
 * References:	APRS Protocol Reference, document version 1.0.1
 *
 *			http://www.aprs.org/doc/APRS101.PDF
 *
 *		APRS SPEC Addendum 1.1
 *
 *			http://www.aprs.org/aprs11.html
 *
 *		APRS SPEC Addendum 1.2
 *
 *			http://www.aprs.org/aprs12.html
 *
 *		"The New n-N Paradigm"
 *
 *			http://www.aprs.org/fix14439.html
 *
 *		Preemptive Digipeating  (new in version 0.8)
 *
 *			http://www.aprs.org/aprs12/preemptive-digipeating.txt
 *		
 *------------------------------------------------------------------*/

#define DIGIPEATER_C


#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
//#include <ctype.h>	/* for isdigit */
#include "regex.h"
#include <sys/unistd.h>

#include "direwolf.h"
#include "ax25_pad.h"
#include "digipeater.h"
#include "textcolor.h"
#include "dedupe.h"
#include "tq.h"


static packet_t digipeat_match (packet_t pp, char *mycall_rec, char *mycall_xmit, 
				regex_t *uidigi, regex_t *uitrace, int to_chan, enum preempt_e preempt);

/*
 * Set by digipeater_init and used later.
 */


static struct digi_config_s my_config;


/*------------------------------------------------------------------------------
 *
 * Name:	digipeater_init
 * 
 * Purpose:	Initialize with stuff from configuration file.
 *
 * Input:	p_digi_config	- Address of structure with all the
 *				necessary configuration details.
 *		
 * Outputs:	Make local copy for later use.
 *		
 * Description:	Called once at application startup time.
 *
 *------------------------------------------------------------------------------*/

void digipeater_init (struct digi_config_s *p_digi_config) 
{
	memcpy (&my_config, p_digi_config, sizeof(my_config));

	dedupe_init (p_digi_config->dedupe_time);
}




/*------------------------------------------------------------------------------
 *
 * Name:	digipeater
 * 
 * Purpose:	Re-transmit packet if it matches the rules.
 *
 * Inputs:	chan	- Radio channel where it was received.
 *		
 * 		pp	- Packet object.
 *		
 * Returns:	None.
 *		
 *
 *------------------------------------------------------------------------------*/



void digipeater (int from_chan, packet_t pp)
{
	int to_chan;
	packet_t result;


	// dw_printf ("digipeater()\n");
	
	assert (from_chan >= 0 && from_chan < my_config.num_chans);


/*
 * First pass:  Look at packets being digipeated to same channel.
 *
 * We want these to get out quickly.
 */

	for (to_chan=0; to_chan<my_config.num_chans; to_chan++) {
	  if (my_config.enabled[from_chan][to_chan]) {
	    if (to_chan == from_chan) {
	      result = digipeat_match (pp, my_config.mycall[from_chan], my_config.mycall[to_chan], 
			&my_config.alias[from_chan][to_chan], &my_config.wide[from_chan][to_chan], 
			to_chan, my_config.preempt[from_chan][to_chan]);
	      if (result != NULL) {
		dedupe_remember (pp, to_chan);
	        tq_append (to_chan, TQ_PRIO_0_HI, result);
	      }
	    }
	  }
	}


/*
 * Second pass:  Look at packets being digipeated to different channel.
 *
 * These are lower priority
 */

	for (to_chan=0; to_chan<my_config.num_chans; to_chan++) {
	  if (my_config.enabled[from_chan][to_chan]) {
	    if (to_chan != from_chan) {
	      result = digipeat_match (pp, my_config.mycall[from_chan], my_config.mycall[to_chan], 
			&my_config.alias[from_chan][to_chan], &my_config.wide[from_chan][to_chan], 
			to_chan, my_config.preempt[from_chan][to_chan]);
	      if (result != NULL) {
		dedupe_remember (pp, to_chan);
	        tq_append (to_chan, TQ_PRIO_1_LO, result);
	      }
	    }
	  }
	}

} /* end digipeater */



/*------------------------------------------------------------------------------
 *
 * Name:	digipeat_match
 * 
 * Purpose:	A simple digipeater for APRS.
 *
 * Input:	pp	- Pointer to a packet object.
 *	
 *		mycall_rec	- Call of my station, with optional SSID,
 *				associated with the radio channel where the 
 *				packet was received.
 *
 *		mycall_rec	- Call of my station, with optional SSID,
 *				associated with the radio channel where the 
 *				packet was received.  Could be the same as
 *				mycall_rec or different.
 *
 *		alias	- Compiled pattern for my station aliases or 
 *				"trapping" (repeating only once).
 *
 *		wide	- Compiled pattern for normal WIDEn-n digipeating.
 *
 *		to_chan		- Channel number that we are transmitting to.
 *				  This is needed to maintain a history for 
 *			 	  removing duplicates during specified time period.
 *
		preempt	- Option for "preemptive" digipeating.
 *		
 * Returns:	Packet object for transmission or NULL.
 *
 * Description:	The packet will be digipeated if the next unused digipeater
 *		field matches one of the following:
 *
 *			- mycall_rec
 *			- udigi list (only once)
 *			- wide list (usual wideN-N rules)
 *
 *------------------------------------------------------------------------------*/

static char *dest_ssid_path[16] = { 	
			"",		/* Use VIA path */
			"WIDE1-1",
			"WIDE2-2",
			"WIDE3-3",
			"WIDE4-4",
			"WIDE5-5",
			"WIDE6-6",
			"WIDE7-7",
			"WIDE1-1",	/* North */
			"WIDE1-1",	/* South */
			"WIDE1-1",	/* East */
			"WIDE1-1",	/* West */
			"WIDE2-2",	/* North */
			"WIDE2-2",	/* South */
			"WIDE2-2",	/* East */
			"WIDE2-2"  };	/* West */
				  

static packet_t digipeat_match (packet_t pp, char *mycall_rec, char *mycall_xmit, 
				regex_t *alias, regex_t *wide, int to_chan, enum preempt_e preempt)
{
	int ssid;
	int r;
	char repeater[AX25_MAX_ADDR_LEN];
	packet_t result = NULL;
	int err;
	char err_msg[100];


/*
 * The spec says:
 *
 * 	The SSID in the Destination Address field of all packets is coded to specify
 * 	the APRS digipeater path.
 * 	If the Destination Address SSID is �0, the packet follows the standard AX.25
 * 	digipeater (�VIA�) path contained in the Digipeater Addresses field of the
 * 	AX.25 frame.
 * 	If the Destination Address SSID is non-zero, the packet follows one of 15
 * 	generic APRS digipeater paths.
 * 
 *
 * What if this is non-zero but there is also a digipeater path?
 * I will ignore this if there is an explicit path.
 *
 * Note that this modifies the input.  But only once!
 * Otherwise we don't want to modify the input because this could be called multiple times.
 */

	if (ax25_get_num_repeaters(pp) == 0 && (ssid = ax25_get_ssid(pp, AX25_DESTINATION)) > 0) {
	  ax25_set_addr(pp, AX25_REPEATER_1, dest_ssid_path[ssid]);
	  ax25_set_ssid(pp, AX25_DESTINATION, 0);
	  /* Continue with general case, below. */
	}

/* 
 * Find the first repeater station which doesn't have "has been repeated" set.
 *
 * r = index of the address position in the frame.
 */
	r = ax25_get_first_not_repeated(pp);

	if (r < AX25_REPEATER_1) {
	  return NULL;
	}

	ax25_get_addr_with_ssid(pp, r, repeater);
	ssid = ax25_get_ssid(pp, r);

#if DEBUG
	text_color_set(DW_COLOR_DEBUG);
	dw_printf ("First unused digipeater is %s, ssid=%d\n", repeater, ssid);
#endif


/*
 * First check for explicit use of my call.
 * In this case, we don't check the history so it would be possible
 * to have a loop (of limited size) if someone constructed the digipeater paths
 * correctly.
 */
	
	if (strcmp(repeater, mycall_rec) == 0) {
	  result = ax25_dup (pp);
	  /* If using multiple radio channels, they */
	  /* could have different calls. */
	  ax25_set_addr (result, r, mycall_xmit);	
	  ax25_set_h (result, r);
	  return (result);
	}

/*
 * Next try to avoid retransmitting redundant information.
 * Duplicates are detected by comparing only:
 *	- source
 *	- destination
 *	- info part
 *	- but none of the digipeaters
 * A history is kept for some amount of time, typically 30 seconds.
 * For efficiency, only a checksum, rather than the complete fields
 * might be kept but the result is the same.
 * Packets transmitted recently will not be transmitted again during
 * the specified time period.
 *
 */


	if (dedupe_check(pp, to_chan)) {
//#if DEBUG
	  /* Might be useful if people are wondering why */
	  /* some are not repeated.  Might cause confusion. */

	  text_color_set(DW_COLOR_INFO);
	  dw_printf ("Digipeater: Drop redundant packet.\n");
//#endif
	  assert (result == NULL);
	  return NULL;
	}

/*
 * For the alias pattern, we unconditionally digipeat it once.
 * i.e.  Just replace it with MYCALL don't even look at the ssid.
 */
	err = regexec(alias,repeater,0,NULL,0);
	if (err == 0) {
	  result = ax25_dup (pp);
	  ax25_set_addr (result, r, mycall_xmit);	
	  ax25_set_h (result, r);
	  return (result);
	}
	else if (err != REG_NOMATCH) {
	  regerror(err, alias, err_msg, sizeof(err_msg));
	  text_color_set (DW_COLOR_ERROR);
	  dw_printf ("%s\n", err_msg);
	}

/* 
 * If preemptive digipeating is enabled, try matching my call 
 * and aliases against all remaining unused digipeaters.
 */


	if (preempt != PREEMPT_OFF) {
	  int r2;

	  for (r2 = r+1; r2 < ax25_get_num_addr(pp); r2++) {
	    char repeater2[AX25_MAX_ADDR_LEN];

	    ax25_get_addr_with_ssid(pp, r2, repeater2);

	    //text_color_set (DW_COLOR_DEBUG);
	    //dw_printf ("test match %d %s\n", r2, repeater2);

	    if (strcmp(repeater2, mycall_rec) == 0 ||
	        regexec(alias,repeater2,0,NULL,0) == 0) {

	      result = ax25_dup (pp);
	      ax25_set_addr (result, r2, mycall_xmit);	
	      ax25_set_h (result, r2);

	      switch (preempt) {
	        case PREEMPT_DROP:	/* remove all prior */
	          while (r2 > AX25_REPEATER_1) {
	            ax25_remove_addr (result, r2-1);
 		    r2--;
	          }
	          break;

	        case PREEMPT_MARK:
	          r2--;
	          while (r2 >= AX25_REPEATER_1 && ax25_get_h(result,r2) == 0) {
	            ax25_set_h (result, r2);
 		    r2--;
	          }
	          break;

		case PREEMPT_TRACE:	/* remove prior unused */
	        default:
	          while (r2 > AX25_REPEATER_1 && ax25_get_h(result,r2-1) == 0) {
	            ax25_remove_addr (result, r2-1);
 		    r2--;
	          }
	          break;
	      }

	      return (result);
	    }
 	  }
	}

/*
 * For the wide pattern, we check the ssid and decrement it.
 */

	err = regexec(wide,repeater,0,NULL,0);
	if (err == 0) {

/*
 * If ssid == 1, we simply replace the repeater with my call and
 *	mark it as being used.
 *
 * Otherwise, if ssid in range of 2 to 7, 
 *	Decrement y and don't mark repeater as being used.
 * 	Insert own call ahead of this one for tracing if we don't already have the 
 *	maximum number of repeaters.
 */

	  if (ssid == 1) {
	    result = ax25_dup (pp);
 	    ax25_set_addr (result, r, mycall_xmit);	
	    ax25_set_h (result, r);
	    return (result);
	  }

	  if (ssid >= 2 && ssid <= 7) {
	    result = ax25_dup (pp);
	    ax25_set_ssid(result, r, ssid-1);	// should be at least 1

	    if (ax25_get_num_repeaters(pp) < AX25_MAX_REPEATERS) {
	      ax25_insert_addr (result, r, mycall_xmit);	
	      ax25_set_h (result, r);
	    }
	    return (result);
	  }
	} 
	else if (err != REG_NOMATCH) {
	  regerror(err, wide, err_msg, sizeof(err_msg));
	  text_color_set (DW_COLOR_ERROR);
	  dw_printf ("%s\n", err_msg);
	}


/*
 * Don't repeat it if we get here.
 */
	assert (result == NULL);
	return NULL;
}



/*-------------------------------------------------------------------------
 *
 * Name:	main
 * 
 * Purpose:	Standalone test case for this funtionality.
 *
 * Usage:	make -f Makefile.<platform> dtest
 *		./dtest 
 *
 *------------------------------------------------------------------------*/

#if TEST

static char mycall[] = "WB2OSZ-9";

static regex_t alias_re;     

static regex_t wide_re;   

static int failed;

static enum preempt_e preempt = PREEMPT_OFF;


static void test (char *in, char *out)
{
	packet_t pp, result;
	//int should_repeat;
	char rec[256];
	char xmit[256];
	unsigned char *pinfo;
	int info_len;
	unsigned char frame[AX25_MAX_PACKET_LEN];
	int frame_len;

	dw_printf ("\n");

/*
 * As an extra test, change text to internal format back to 
 * text again to make sure it comes out the same.
 */
	pp = ax25_from_text (in, 1);
	assert (pp != NULL);

	ax25_format_addrs (pp, rec);
	info_len = ax25_get_info (pp, &pinfo);
	strcat (rec, (char*)pinfo);

	if (strcmp(in, rec) != 0) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Text/internal/text error %s -> %s\n", in, rec);
	}

/*
 * Just for more fun, write as the frame format, read it back
 * again, and make sure it is still the same.
 */

	frame_len = ax25_pack (pp, frame);
	ax25_delete (pp);

	pp = ax25_from_frame (frame, frame_len, 50);
	ax25_format_addrs (pp, rec);
	info_len = ax25_get_info (pp, &pinfo);
	strcat (rec, (char*)pinfo);

	if (strcmp(in, rec) != 0) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("internal/frame/internal/text error %s -> %s\n", in, rec);
	}

/*
 * On with the digipeater test.
 */	
	
	text_color_set(DW_COLOR_REC);
	dw_printf ("Rec\t%s\n", rec);

	result = digipeat_match (pp, mycall, mycall, &alias_re, &wide_re, 0, preempt);
	
	if (result != NULL) {

	  dedupe_remember (result, 0);
	  ax25_format_addrs (result, xmit);
	  info_len = ax25_get_info (result, &pinfo);
	  strcat (xmit, (char*)pinfo);
	  ax25_delete (result);
	}
	else {
	  strcpy (xmit, "");
	}

	text_color_set(DW_COLOR_XMIT);
	dw_printf ("Xmit\t%s\n", xmit);
	
	if (strcmp(xmit, out) == 0) {
	  text_color_set(DW_COLOR_INFO);
	  dw_printf ("OK\n");
	}
	else {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Expect\t%s\n", out);
 	  failed++;
	}

	dw_printf ("\n");
}

int main (int argc, char *argv[])
{
	int e;
	failed = 0;
	char message[256];

	dedupe_init (4);

/* 
 * Compile the patterns. 
 */
	e = regcomp (&alias_re, "^WIDE[4-7]-[1-7]|CITYD$", REG_EXTENDED|REG_NOSUB);
	if (e != 0) {
	  regerror (e, &alias_re, message, sizeof(message));
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("\n%s\n\n", message);
	  exit (1);
	}

	e = regcomp (&wide_re, "^WIDE[1-7]-[1-7]$|^TRACE[1-7]-[1-7]$|^MA[1-7]-[1-7]$", REG_EXTENDED|REG_NOSUB);
	if (e != 0) {
	  regerror (e, &wide_re, message, sizeof(message));
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("\n%s\n\n", message);
	  exit (1);
	}

/*
 * Let's start with the most basic cases.
 */

	test (	"W1ABC>TEST01,TRACE3-3:",
		"W1ABC>TEST01,WB2OSZ-9*,TRACE3-2:");

	test (	"W1ABC>TEST02,WIDE3-3:",
		"W1ABC>TEST02,WB2OSZ-9*,WIDE3-2:");

	test (	"W1ABC>TEST03,WIDE3-2:",
		"W1ABC>TEST03,WB2OSZ-9*,WIDE3-1:");

	test (	"W1ABC>TEST04,WIDE3-1:",
		"W1ABC>TEST04,WB2OSZ-9*:");

/*
 * Look at edge case of maximum number of digipeaters.
 */
	test (	"W1ABC>TEST11,R1,R2,R3,R4,R5,R6*,WIDE3-3:",
		"W1ABC>TEST11,R1,R2,R3,R4,R5,R6,WB2OSZ-9*,WIDE3-2:");

	test (	"W1ABC>TEST12,R1,R2,R3,R4,R5,R6,R7*,WIDE3-3:",
		"W1ABC>TEST12,R1,R2,R3,R4,R5,R6,R7*,WIDE3-2:");

	test (	"W1ABC>TEST13,R1,R2,R3,R4,R5,R6,R7*,WIDE3-1:",
		"W1ABC>TEST13,R1,R2,R3,R4,R5,R6,R7,WB2OSZ-9*:");

/*
 * "Trap" large values of "N" by repeating only once.
 */
	test (	"W1ABC>TEST21,WIDE4-4:",
		"W1ABC>TEST21,WB2OSZ-9*:");

	test (	"W1ABC>TEST22,WIDE7-7:",
		"W1ABC>TEST22,WB2OSZ-9*:");

/*
 * Only values in range of 1 thru 7 are valid.
 */
	test (	"W1ABC>TEST31,WIDE0-4:",
		"");

	test (	"W1ABC>TEST32,WIDE8-4:",
		"");

	test (	"W1ABC>TEST33,WIDE2:",
		"");


/*
 * and a few cases actually heard.
 */

	test (	"WA1ENO>FN42ND,W1MV-1*,WIDE3-2:",
		"WA1ENO>FN42ND,W1MV-1,WB2OSZ-9*,WIDE3-1:");

	test (	"W1ON-3>BEACON:",
		"");

	test (	"W1CMD-9>TQ3Y8P,N1RCW-2,W1CLA-1,N8VIM,WIDE2*:",
		"");

	test (	"W1CLA-1>APX192,W1GLO-1,WIDE2*:",
		"");

	test (	"AC1U-9>T2TX4S,AC1U,WIDE1,N8VIM*,WIDE2-1:",
		"AC1U-9>T2TX4S,AC1U,WIDE1,N8VIM,WB2OSZ-9*:");

/*
 * Someone is still using the old style and will probably be disappointed.
 */

	test (	"K1CPD-1>T2SR5R,RELAY*,WIDE,WIDE,SGATE,WIDE:",
		"");


/* 
 * Change destination SSID to normal digipeater if none specified.
 */
	test (	"W1ABC>TEST-3:",
		"W1ABC>TEST,WB2OSZ-9*,WIDE3-2:");

	test (	"W1DEF>TEST-3,WIDE2-2:",
		"W1DEF>TEST-3,WB2OSZ-9*,WIDE2-1:");

/*
 * Drop duplicates within specified time interval.
 * Only the first 1 of 3 should be retransmitted.
 */

	test (	"W1XYZ>TEST,R1*,WIDE3-2:info1",
		"W1XYZ>TEST,R1,WB2OSZ-9*,WIDE3-1:info1");

	test (	"W1XYZ>TEST,R2*,WIDE3-2:info1",
		"");

	test (	"W1XYZ>TEST,R3*,WIDE3-2:info1",
		"");

/*
 * Allow same thing after adequate time.
 */
	SLEEP_SEC (5);

	test (	"W1XYZ>TEST,R3*,WIDE3-2:info1",
		"W1XYZ>TEST,R3,WB2OSZ-9*,WIDE3-1:info1");

/*
 * Although source and destination match, the info field is different.
 */

	test (	"W1XYZ>TEST,R1*,WIDE3-2:info4",
		"W1XYZ>TEST,R1,WB2OSZ-9*,WIDE3-1:info4");

	test (	"W1XYZ>TEST,R1*,WIDE3-2:info5",
		"W1XYZ>TEST,R1,WB2OSZ-9*,WIDE3-1:info5");

	test (	"W1XYZ>TEST,R1*,WIDE3-2:info6",
		"W1XYZ>TEST,R1,WB2OSZ-9*,WIDE3-1:info6");

/*
 * New in version 0.8.
 * "Preemptive" digipeating looks ahead beyond the first unused digipeater.
 */

	test (	"W1ABC>TEST11,CITYA*,CITYB,CITYC,CITYD,CITYE:off",
		"");

	preempt = PREEMPT_DROP;

	test (	"W1ABC>TEST11,CITYA*,CITYB,CITYC,CITYD,CITYE:drop",
		"W1ABC>TEST11,WB2OSZ-9*,CITYE:drop");

	preempt = PREEMPT_MARK;

	test (	"W1ABC>TEST11,CITYA*,CITYB,CITYC,CITYD,CITYE:mark1",
		"W1ABC>TEST11,CITYA,CITYB,CITYC,WB2OSZ-9*,CITYE:mark1");

	test (	"W1ABC>TEST11,CITYA*,CITYB,CITYC,WB2OSZ-9,CITYE:mark2",
		"W1ABC>TEST11,CITYA,CITYB,CITYC,WB2OSZ-9*,CITYE:mark2");

	preempt = PREEMPT_TRACE;

	test (	"W1ABC>TEST11,CITYA*,CITYB,CITYC,CITYD,CITYE:trace1",
		"W1ABC>TEST11,CITYA,WB2OSZ-9*,CITYE:trace1");

	test (	"W1ABC>TEST11,CITYA*,CITYB,CITYC,CITYD:trace2",
		"W1ABC>TEST11,CITYA,WB2OSZ-9*:trace2");

	test (	"W1ABC>TEST11,CITYB,CITYC,CITYD:trace3",
		"W1ABC>TEST11,WB2OSZ-9*:trace3");

	test (	"W1ABC>TEST11,CITYA*,CITYW,CITYX,CITYY,CITYZ:nomatch",
		"");


/* 
 * Did I miss any cases?
 */

	if (failed == 0) {
	  dw_printf ("SUCCESS -- All digipeater tests passed.\n");
	}
	else {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("ERROR - %d digipeater tests failed.\n", failed);
	}

	return ( failed != 0 ); 

} /* end main */

#endif  /* if TEST */

/* end digipeater.c */
