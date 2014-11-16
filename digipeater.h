

#ifndef DIGIPEATER_H
#define DIGIPEATER_H 1

#include "regex.h"

#include "direwolf.h"		/* for MAX_CHANS */
#include "ax25_pad.h"		/* for packet_t */

/*
 * Information required for digipeating.
 *
 * The configuration file reader fills in this information
 * and it is passed to digipeater_init at application start up time.
 */


struct digi_config_s {

	int	num_chans;

	char mycall[MAX_CHANS][AX25_MAX_ADDR_LEN];	/* Call associated */
					/* with each of the radio channels.  */
					/* Could be the same or different. */

	int	dedupe_time;	/* Don't digipeat duplicate packets */
				/* within this number of seconds. */

#define DEFAULT_DEDUPE 30

/*
 * Rules for each of the [from_chan][to_chan] combinations.
 */

	regex_t	alias[MAX_CHANS][MAX_CHANS];

	regex_t	wide[MAX_CHANS][MAX_CHANS];

	int	enabled[MAX_CHANS][MAX_CHANS];

	enum preempt_e { PREEMPT_OFF, PREEMPT_DROP, PREEMPT_MARK, PREEMPT_TRACE } preempt[MAX_CHANS][MAX_CHANS];

};

/*
 * Call once at application start up time.
 */

extern void digipeater_init (struct digi_config_s *p_digi_config);

/*
 * Call this for each packet received.
 * Suitable packets will be queued for transmission.
 */

extern void digipeater (int from_chan, packet_t pp);

#endif 

/* end digipeater.h */

