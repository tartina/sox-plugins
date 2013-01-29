/*
    LibSox limiter
    Copyright (C) 2013 Guido Aulisi <guido.aulisi@gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <string.h>
#include <math.h>

#include "sox_i.h"

#define LOOKAHEAD_TIME .025f
#define LIMITER_USAGE "threshold (db)"
#define NUMBER_OF_CHANNELS 2

#define DB_CO(g) ((g) > -90.0f ? powf(10.0f, (g) * 0.05f) : 0.0f)
#define CO_DB(v) (20.0f * log10f(v))

typedef struct {
  double threshold;           /* Max level */
  double gain;                /* Current gain of all channel */
  sox_sample_t *delay_buf;    /* Old samples, used for delay processing */
  ptrdiff_t delay_buf_size;   /* Size of delay_buf in samples */
  ptrdiff_t delay_buf_index;  /* Index into delay_buf */
	ptrdiff_t zero_cross_start; /* Start of waveform: between start and end we apply gain */
	ptrdiff_t zero_cross_end;   /* End of waveform */
  ptrdiff_t delay_buf_cnt;    /* No. of active entries in delay_buf */
  int delay_buf_full;         /* Shows buffer situation (important for drain) */
} limiter_t;

static int getopts(sox_effect_t * effp, int argc, char * * argv)
{
	limiter_t * l = (limiter_t *) effp->priv;

	if (argc < 2) return lsx_usage(effp);

  if (sscanf(argv[1], "%lf", &l->threshold) != 1) {
    lsx_fail("syntax error trying to read threshold");
    return SOX_EOF;
	}

	if (l->threshold > 0 || l->threshold < -40) {
    lsx_fail("threshold cannot be > 0 or < -40");
    return SOX_EOF;	
	}

	/* Convert db to linear value */
	l->threshold = DB_CO(l->threshold);

  return SOX_SUCCESS;
}

static int start(sox_effect_t * effp)
{
	limiter_t * l = (limiter_t *) effp->priv;

	/* This limiter works only with 2 channels */
	if (effp->out_signal.channels != NUMBER_OF_CHANNELS) {
		lsx_fail("Only 2 channels");
		return SOX_EOF;
	}

	/* Allocate lookahead buffer */
  l->delay_buf_size = LOOKAHEAD_TIME * effp->out_signal.rate * NUMBER_OF_CHANNELS;
  if (l->delay_buf_size > 0)
    if ( (l->delay_buf = lsx_calloc((size_t)l->delay_buf_size, sizeof(*l->delay_buf))) ) {
		  l->delay_buf_index = 0;
  		l->delay_buf_cnt = 0;
  		l->delay_buf_full= 0;
			return SOX_SUCCESS;
		}

  return SOX_EOF;
}

static sox_sample_t * find_next_zero_crossing(const sox_sample_t *ibuf, size_t size)
{
	size_t i;
	sox_sample_t * zero_crossing = NULL;

	for (zero_crossing = ibuf + NUMBER_OF_CHANNELS, i = NUMBER_OF_CHANNELS; i < size / NUMBER_OF_CHANNELS; i++, zero_crossing += NUMBER_OF_CHANNELS)
	{
		if ((*zero_crossing) < 0 && *(zero_crossing + NUMBER_OF_CHANNELS) >=0) break;
	}

	return zero_crossing;
}

static sox_sample_t * find_max_overflow(const sox_sample_t *ibuf, const sox_sample_t *end, double limit)
{
	sox_sample_t * overflow = NULL;
	sox_sample_t * max = NULL;
	double current_value = 0.0f, max_value = 0.0f;

	limit = abs(limit);

	for (overflow = ibuf; overflow < end; overflow++) {
		current_value = abs(*overflow);
		if (current_value > limit && current_value > max_value) {
			max_value = current_value;
			max = overflow;
		} 
	}
	
	return max;
}

static int flow(sox_effect_t * effp, const sox_sample_t *ibuf, sox_sample_t *obuf,
                    size_t *isamp, size_t *osamp)
{
	limiter_t * l = (limiter_t *) effp->priv;
	size_t length = (*isamp > *osamp) ? *osamp : *isamp;
	size_t idone, odone;
	sox_sample_t * zero_cross;
	sox_sample_t * max;
	double factor;

	idone = odone = 0;

	while (idone < length)
	{
		zero_cross = find_next_zero_crossing(ibuf, (*isamp) - idone);
		if (! zero_cross) break;
		max = find_max_overflow(ibuf, zero_cross, l->threshold);

		if (max) { /* We have to limit */
			/* Calculate factor */
			factor = l->threshold / (*max);
			for (; ibuf < zero_cross; ibuf++, obuf++, idone++, odone++) *obuf = (*ibuf) * factor;
		} else { /* Copy input to output */
			for (; ibuf < zero_cross; ibuf++, obuf++, idone++, odone++) *obuf = *ibuf;
		}
	}

  *isamp = idone; *osamp = odone;
  return SOX_SUCCESS;
}

static int drain(sox_effect_t * effp, sox_sample_t *obuf, size_t *osamp)
{
  return SOX_EOF;
}

static int stop(sox_effect_t * effp)
{
	limiter_t * l = (limiter_t *) effp->priv;

  free(l->delay_buf);
  return SOX_SUCCESS;
}

static int lsx_kill(sox_effect_t * effp)
{
  return SOX_SUCCESS;
}

sox_effect_handler_t const * lsx_limiter_effect_fn(void)
{
  static sox_effect_handler_t handler = {
    "limiter", LIMITER_USAGE, SOX_EFF_MCHAN | SOX_EFF_GAIN | SOX_EFF_ALPHA,
    getopts, start, flow, drain, stop, lsx_kill, sizeof(limiter_t)
  };
  return &handler;
}

