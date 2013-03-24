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

#define LOOKAHEAD_TIME .2f	/* in seconds */
#define LIMITER_USAGE "threshold (db)"
#define NUMBER_OF_CHANNELS 2

#define DB_CO(g) ((g) > -90.0f ? powf(10.0f, (g) * 0.05f) : 0.0f)
#define CO_DB(v) (20.0f * log10f(v))

typedef struct {
	sox_sample_t threshold;	/* Max level */
	double gain;		/* Current gain */
	sox_sample_t *buffer;	/* Audio buffer */
	size_t buffer_size;	/* Size of audio buffer */
	sox_sample_t *position;	/* Audio buffer actual position */
	size_t buffer_active;	/* Number of samples in audio buffer */
	uint32_t actions;	/* Number of limiter actions */
} limiter_t;

static int getopts(sox_effect_t * effp, int argc, char * *argv)
{
	float threshold;
	limiter_t *l = (limiter_t *) effp->priv;

	--argc, ++argv;
	if (argc != 1)
		return lsx_usage(effp);

	if (sscanf(argv[0], "%f", &threshold) != 1) {
		lsx_fail("syntax error trying to read threshold");
		return SOX_EOF;
	}

	if (threshold > 0 || threshold < -40) {
		lsx_fail("threshold cannot be > 0 or < -40");
		return SOX_EOF;
	}

	/* Convert db to linear value */
	l->threshold = DB_CO(threshold) * SOX_SAMPLE_MAX;

	return SOX_SUCCESS;
}

static int start(sox_effect_t * effp)
{
	limiter_t *l = (limiter_t *) effp->priv;

	/* This limiter works only with 2 channels */
	if (effp->out_signal.channels != NUMBER_OF_CHANNELS) {
		lsx_fail("Only 2 channels");
		return SOX_EOF;
	}

	l->gain = 1.0f;
	l->actions = 0;

	/* Allocate the delay buffer */
	l->buffer_size = LOOKAHEAD_TIME * effp->out_signal.rate * NUMBER_OF_CHANNELS;
	l->buffer_active = 0;
	if (l->buffer_size > 0)
		if ((l->buffer = lsx_calloc((size_t) l->buffer_size, sizeof(sox_sample_t))))
			return SOX_SUCCESS;

	lsx_fail("Cannot allocate buffer");
	return SOX_EOF;
}

static sox_sample_t *find_next_zero_crossing(const sox_sample_t * ibuf, size_t size)
{
	size_t i;
	sox_sample_t *zero_crossing = NULL;
	sox_sample_t *result = NULL;

	if (size == 0)
		return NULL;

	for (zero_crossing = ibuf + NUMBER_OF_CHANNELS, i = NUMBER_OF_CHANNELS;
	     i < (size / NUMBER_OF_CHANNELS); i += NUMBER_OF_CHANNELS, zero_crossing += NUMBER_OF_CHANNELS) {
		if ((*zero_crossing) <= 0 && (*(zero_crossing + NUMBER_OF_CHANNELS)) >= 0) {
			result = zero_crossing;
			break;
		}
	}

	return result;
}

static sox_sample_t *find_max_overflow(const sox_sample_t * ibuf, const sox_sample_t * end, sox_sample_t limit)
{
	sox_sample_t *overflow = NULL;
	sox_sample_t *max = NULL;
	sox_sample_t current_value = 0, max_value = 0;

	for (overflow = ibuf; overflow < end; overflow++) {
		current_value = abs(*overflow);
		if (current_value > limit && current_value > max_value) {
			max_value = current_value;
			max = overflow;
		}
	}

	return max;
}

static int flow(sox_effect_t * effp, const sox_sample_t * ibuf, sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
	limiter_t *l = (limiter_t *) effp->priv;
	size_t idone, odone, remaining;
	sox_sample_t *zero_cross;
	sox_sample_t *max, *buffer_max;

	/* Safe control */
	if (l->buffer_active > *osamp) {
		lsx_fail("Internal buffer too big");
		return -SOX_ENOTSUP;
	}

	idone = odone = 0;

	/* Process our buffer */
	if (l->buffer_active > 0) {
		zero_cross = find_next_zero_crossing(l->position, l->buffer_active);
		while (zero_cross) {
			max = find_max_overflow(l->buffer, zero_cross, l->threshold);
			if (max) {
				l->actions++;
				l->gain = (double)l->threshold / (double)abs(*max);
				for (; odone < *osamp && l->position < zero_cross; odone++, l->position++, obuf++, l->buffer_active--)
					*obuf = (double)(*l->position) * l->gain;
			} else {
				l->gain = 1.0f;
				for (; odone < *osamp && l->position < zero_cross; odone++, l->position++, obuf++, l->buffer_active--)
					*obuf = *(l->position);
			}
			zero_cross = find_next_zero_crossing(l->position, l->buffer_active);
		}
	}

	/* Process our buffer with in buffer; there should be no zero cross in our buffer! */
	if (l->buffer_active > 0) {
		remaining = *isamp - idone;
		zero_cross = find_next_zero_crossing(ibuf, remaining);
		if (zero_cross) {
			max = find_max_overflow(ibuf, zero_cross, l->threshold);
			buffer_max = find_max_overflow(l->position, l->position + l->buffer_active, l->threshold);
			/* Find max of max */
			if (buffer_max) {
				if (max) {
					if (abs(*buffer_max) > abs(*max))
						max = buffer_max;
				} else
					max = buffer_max;
			}

			if (max) {
				l->actions++;
				l->gain = (double)l->threshold / (double)abs(*max);
				/* Process our buffer */
				for (; odone < *osamp && l->position < l->position + l->buffer_active;
				     odone++, l->position++, obuf++, l->buffer_active--)
					*obuf = (double)(*l->position) * l->gain;
				/* Process in buffer */
				for (; odone < *osamp && idone < *isamp && ibuf < zero_cross; odone++, obuf++, idone++, ibuf++)
					*obuf = (double)(*ibuf) * l->gain;
			} else {
				l->gain = 1.0f;
				/* Process our buffer */
				for (; odone < *osamp && l->position < l->position + l->buffer_active;
				     odone++, l->position++, obuf++, l->buffer_active--)
					*obuf = *(l->position);
				/* Process in buffer */
				for (; odone < *osamp && idone < *isamp && ibuf < zero_cross; odone++, obuf++, idone++, ibuf++)
					*obuf = *ibuf;
			}
		}

		*isamp = idone;
		*osamp = odone;
		return SOX_SUCCESS;
	}

	/* Process in buffer only */
	remaining = *isamp - idone;
	zero_cross = find_next_zero_crossing(ibuf, remaining);
	while (idone < *isamp && odone < *osamp && zero_cross) {
		max = find_max_overflow(ibuf, zero_cross, l->threshold);
		if (max) {
			l->actions++;
			l->gain = (double)l->threshold / (double)abs(*max);
			for (; ibuf < zero_cross && idone < *isamp && odone < *osamp; ibuf++, obuf++, idone++, odone++)
				*obuf = (double)(*ibuf) * l->gain;
		} else {
			l->gain = 1.0f;
			for (; ibuf < zero_cross && idone < *isamp && odone < *osamp; ibuf++, obuf++, idone++, odone++)
				*obuf = *ibuf;
		}

		remaining = *isamp - idone;
		zero_cross = find_next_zero_crossing(ibuf, remaining);
	}

	/* Safe check */
	if (l->buffer_active != 0 && idone < *isamp) {
		lsx_fail("Can't save input data, buffer not empty");
		return SOX_EOF;
	}

	/* Copy ibuf to buffer for next run */
	if (remaining < l->buffer_size && l->buffer_active == 0) {
		memcpy(l->buffer, ibuf, remaining * sizeof(ibuf));
		l->buffer_active = remaining;
		l->position = l->buffer;
	} else
		*isamp = idone;

	*osamp = odone;

	return SOX_SUCCESS;
}

static int drain(sox_effect_t * effp, sox_sample_t * obuf, size_t * osamp)
{
	size_t i;
	limiter_t *l = (limiter_t *) effp->priv;

	if (l->buffer_active < *osamp) {
		for (i = 0; i < l->buffer_active; i++, obuf++)
			*obuf = (double)(l->position[i]) * l->gain;
		*osamp = i;
		return SOX_EOF;
	}

	lsx_fail("Internal buffer too big in drain");
	return -SOX_ENOTSUP;
}

static int stop(sox_effect_t * effp)
{
	limiter_t *l = (limiter_t *) effp->priv;

	free(l->buffer);

	lsx_report("We have lowered gain %u times", l->actions);

	return SOX_SUCCESS;
}

sox_effect_handler_t const *lsx_limiter_effect_fn(void)
{
	static sox_effect_handler_t handler = {
		"limiter", LIMITER_USAGE,
		SOX_EFF_MCHAN | SOX_EFF_GAIN | SOX_EFF_ALPHA,
		getopts, start, flow, drain, stop, NULL, sizeof(limiter_t)
	};
	return &handler;
}
