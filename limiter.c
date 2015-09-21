/*
    LibSox limiter
    Copyright (C) 2013 Guido Aulisi guido.aulisi@gmail.com

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
#include <unistd.h>
#include <sys/mman.h>
#include <stdlib.h>

#include "sox_i.h"

#define LOOKAHEAD_TIME 1.0f	/* in seconds */
#define LIMITER_USAGE "threshold (db)"
#define NUMBER_OF_CHANNELS 2 /* TESTED ONLY WITH 2 CHANNELS */

#define DB_CO(g) ((g) > -90.0f ? powf(10.0f, (g) * 0.05f) : 0.0f)
#define CO_DB(v) (20.0f * log10f(v))

/* Define ZERO_CROSSING_CHECK_OTHER_CHANNELS if you want to check the other channel(s) for ZERO CROSSING detection */
#define ZERO_CROSSING_CHECK_OTHER_CHANNELS
/* If checking the other channels(s), they must be less than this values to be a ZERO CROSSING (-40 dB) */
static const sox_sample_t MAX_ZERO_CROSSING_VALUE = (0.01f * SOX_SAMPLE_MAX);

// Ring buffer
typedef struct {
	sox_sample_t *data;
	size_t size;						/* Total size of buffer in samples */
	size_t available;				/* Number of samples in the buffer */
	size_t processed;				/* Number of sample already processede, must be < available */
	sox_sample_t *position;	/* Audio buffer actual position */
} ring_buffer_t;

typedef struct {
	sox_sample_t threshold;	/* Max level */
	double gain;						/* Current gain */
	ring_buffer_t *rbuffer;	/* Audio buffer */
	uint32_t actions;				/* Number of limiter actions */
	uint32_t slices;				/* Number of slices found by the zero crossing detector */
} limiter_t;

static ring_buffer_t *create_ring_buffer(const size_t requested_size /* in bytes */)
{
	int fd;
	uint8_t *the_data, *address;
	ring_buffer_t *the_buffer;
	char file_name[] = "/tmp/lim-XXXXXX";

	/* Try to map double size memory */
	the_data = mmap(NULL, requested_size * 2, PROT_NONE,
		MAP_PRIVATE|MAP_ANONYMOUS, -1, (off_t)0);

	if (the_data == MAP_FAILED) return NULL;

	fd = mkstemp(file_name);
	if (fd <0) {
		munmap(the_data, requested_size * 2);
		return NULL;
	}
	if (ftruncate(fd, requested_size) < 0) {
		munmap(the_data, requested_size * 2);
		close(fd);
		return NULL;
	}

	address = mmap(the_data, requested_size, PROT_READ|PROT_WRITE,
		MAP_FIXED|MAP_SHARED, fd, (off_t)0);

	if (address == MAP_FAILED) {
		munmap(the_data, requested_size * 2);
		close(fd);
		return NULL;
	}

	the_data = address;

	address = mmap(the_data + requested_size, requested_size, PROT_READ|PROT_WRITE,
		MAP_FIXED|MAP_SHARED, fd, (off_t)0);
	if (address == MAP_FAILED) {
		close(fd);
		munmap(the_data, requested_size);
		return NULL;
	}

	close(fd);

	the_buffer = (ring_buffer_t *) malloc(sizeof(ring_buffer_t));
	if (the_buffer) {
		the_buffer->data = (sox_sample_t *)the_data;
		the_buffer->size = requested_size / sizeof(sox_sample_t);
		the_buffer->available = 0;
		the_buffer->processed = 0;
		the_buffer->position = (sox_sample_t *)the_data;
	} else munmap(the_data, requested_size * 2);

	return the_buffer;
}
static void delete_ring_buffer(ring_buffer_t *buffer)
{
	munmap(buffer->data, buffer->size * 2 * sizeof(sox_sample_t));
	free(buffer);
}
static int ring_buffer_write(ring_buffer_t* const buffer, const sox_sample_t *input, const size_t count)
{
	sox_sample_t *destination;

	if (count == 0) return 0; /* Nothing to do */
	if (count > (buffer->size - buffer->available)) return -1;

	destination = buffer->position + buffer->available;
	if (destination >= buffer->data + buffer->size)
		destination -= buffer->size;
	buffer->available += count;

	memcpy(destination, input, count * sizeof(sox_sample_t));

	return 0;
}
/*
	Return the data pointer if we have count samples to read
	It doesn't remove the data from the buffer
*/
static sox_sample_t *ring_buffer_read(const ring_buffer_t* const buffer, size_t count)
{
	if (count > buffer->available) return NULL;

	return buffer->position;
}
/*
	Remove count processed samples from the buffer
*/
static int ring_buffer_pop(ring_buffer_t* const buffer, size_t count)
{
	if (count > buffer->processed) return -1;
	buffer->position += count;
	buffer->available -= count;
	buffer->processed -= count;

	if (buffer->position >= buffer->data + buffer->size)
		buffer->position -= buffer->size;
	return 0;
}
/*
	Mark processed data
*/
static int ring_buffer_mark_processed(ring_buffer_t* const buffer, size_t count)
{
	if (count > (buffer->available - buffer->processed)) return -1;
	buffer->processed += count;
	return 0;
}
/*
	Get free buffer size
*/
static size_t ring_buffer_get_free(const ring_buffer_t* const buffer)
{
	return buffer->size - buffer->available;
}
/*
	Get start pointer of unprocessed data
*/
static sox_sample_t *ring_buffer_get_start_unprocessed(const ring_buffer_t* const buffer)
{
	sox_sample_t *result;
	result = buffer->position + buffer->processed;
	if(result >= buffer->data + buffer->size) result -= buffer->size;
	return result;
}
/*
	Get unprocessed buffer size
*/
static size_t ring_buffer_get_unprocessed(const ring_buffer_t* const buffer)
{
	return buffer->available - buffer->processed;
}

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

	if (threshold > 0.0f || threshold < -40.0f) {
		lsx_fail("threshold cannot be > 0 or < -40");
		return SOX_EOF;
	}

	/* Convert db to linear value */
	l->threshold = DB_CO(threshold) * SOX_SAMPLE_MAX;

	return SOX_SUCCESS;
}

static int start(sox_effect_t * effp)
{
	size_t buffer_size, pagesize, real_size;
	unsigned int reminder;

	limiter_t *l = (limiter_t *) effp->priv;

	/* This limiter works only with 2 channels */
	if (effp->out_signal.channels != NUMBER_OF_CHANNELS) {
		lsx_fail("This limiter works only with 2 channels audio");
		return SOX_EOF;
	}

	l->gain = 1.0f;
	l->actions = 0;
	l->slices = 0;

	/* Allocate the lookahead buffer */
	buffer_size = LOOKAHEAD_TIME * effp->out_signal.rate * NUMBER_OF_CHANNELS;

	pagesize = (size_t) sysconf(_SC_PAGESIZE);
	real_size = buffer_size * sizeof(sox_sample_t);

	/* Check if requested_size is multiple of pagesize and sizeof(sox_sample_t) */
	if ((reminder = real_size % pagesize))
		real_size += pagesize - reminder;

	if ((reminder = real_size % (sizeof(sox_sample_t) * NUMBER_OF_CHANNELS) ))
		return SOX_EOF;

	if (buffer_size > 0)
		if ((l->rbuffer = create_ring_buffer(real_size)))
			return SOX_SUCCESS;

	lsx_fail("Cannot allocate buffer");
	return SOX_EOF;
}

static const sox_sample_t *find_next_zero_crossing(const sox_sample_t * ibuf, size_t size)
{
	size_t i;
	const sox_sample_t *zero_crossing = NULL;
	const sox_sample_t *result = NULL;
#ifdef ZERO_CROSSING_CHECK_OTHER_CHANNELS
	const sox_sample_t *k = NULL; /* Pointer to check other channel(s) */
	unsigned short fake = 0;
#endif

	if (size == 0) return NULL;

	for (zero_crossing = ibuf, i = NUMBER_OF_CHANNELS;
	     i < (size / NUMBER_OF_CHANNELS); i += NUMBER_OF_CHANNELS, zero_crossing += NUMBER_OF_CHANNELS) {
		if ((*zero_crossing) <= 0 && (*(zero_crossing + NUMBER_OF_CHANNELS)) > 0) {
#ifdef ZERO_CROSSING_CHECK_OTHER_CHANNELS
			fake = 0;
			for (k = zero_crossing + 1; k < zero_crossing + NUMBER_OF_CHANNELS; ++k) {
				if (abs(*k) > MAX_ZERO_CROSSING_VALUE) {
					fake = 1;
					break;
				}
			}
			if (!fake) {
#endif
			result = zero_crossing + NUMBER_OF_CHANNELS;
			break;
#ifdef ZERO_CROSSING_CHECK_OTHER_CHANNELS
			}
#endif
		}
	}
	return result;
}

static const sox_sample_t *find_max_overflow(const sox_sample_t * ibuf, const sox_sample_t * end, sox_sample_t limit)
{
	const sox_sample_t *overflow = NULL;
	const sox_sample_t *max = NULL;
	sox_sample_t current_value = 0, max_value = 0;

	for (overflow = ibuf; overflow < end; ++overflow) {
		current_value = abs(*overflow);
		if (current_value > limit && current_value > max_value) {
			max_value = current_value;
			max = overflow;
		}
	}

	return max;
}

static void process_our_buffer(ring_buffer_t* const buffer, limiter_t* const l)
{
	const sox_sample_t *zero_cross;
	const sox_sample_t *max;
	sox_sample_t *index;

	zero_cross = find_next_zero_crossing(
		ring_buffer_get_start_unprocessed(buffer),
		ring_buffer_get_unprocessed(buffer));
	while (zero_cross) {
		++(l->slices);
		max = find_max_overflow(ring_buffer_get_start_unprocessed(buffer), zero_cross, l->threshold);
		if (max) {
			++(l->actions);
			l->gain = (double)l->threshold / (double)abs(*max);
			for (index = ring_buffer_get_start_unprocessed(buffer); index < zero_cross; ++index)
				*index = (double)(*index) * l->gain;
		} else l->gain = 1.0f;
		ring_buffer_mark_processed(buffer, zero_cross - ring_buffer_get_start_unprocessed(buffer));
		zero_cross = find_next_zero_crossing(
			ring_buffer_get_start_unprocessed(buffer),
			ring_buffer_get_unprocessed(buffer));
	}
}

static int flow(sox_effect_t * effp, const sox_sample_t * ibuf, sox_sample_t * obuf, size_t * isamp, size_t * osamp)
{
	limiter_t *l = (limiter_t *) effp->priv;
	ring_buffer_t *buffer = l->rbuffer;
	size_t idone, odone;

	idone = odone = 0;

	/* Copy processed buffer to output */
	if (buffer->processed > 0) {
		odone = min(buffer->processed, *osamp);
		if (odone > 0) {
			memcpy(obuf, ring_buffer_read(buffer, odone), odone * sizeof(sox_sample_t));
			ring_buffer_pop(buffer, odone);
		}
	}
	*osamp = odone;

	/* Copy in buffer to our buffer */
	idone = min(ring_buffer_get_free(buffer), *isamp);
	if (ring_buffer_write(buffer, ibuf, idone) == -1) {
		lsx_fail("Can't save input data, buffer full");
		return SOX_EOF;
	}
	*isamp = idone;

	/* Process our buffer */
	process_our_buffer(buffer, l);

	return SOX_SUCCESS;
}

static int drain(sox_effect_t * effp, sox_sample_t * obuf, size_t * osamp)
{
	limiter_t *l = (limiter_t *) effp->priv;
	ring_buffer_t *buffer = l->rbuffer;
	size_t odone;
	sox_sample_t *index;
	size_t i;

	odone = 0;

	/* Process our buffer */
	process_our_buffer(buffer, l);

	/* Process remaining data using current gain */
	if (buffer->available > 0) {
		for (i = buffer->available, index = ring_buffer_get_start_unprocessed(buffer);
			i > 0; --i, ++index) *index = (double)(*index) * l->gain;
		ring_buffer_mark_processed(buffer, buffer->available);
	}

	/* Copy processed buffer to output */
	if (buffer->processed > 0) {
		odone = min(buffer->processed, *osamp);
		if (odone > 0) memcpy(obuf, ring_buffer_read(buffer, odone), odone * sizeof(sox_sample_t));
		ring_buffer_pop(buffer, odone);
	}
	*osamp = odone;

	return SOX_SUCCESS;
}

static int stop(sox_effect_t * effp)
{
	limiter_t *l = (limiter_t *) effp->priv;

	delete_ring_buffer(l->rbuffer);

	lsx_report("We have lowered gain %u times", l->actions);
	lsx_report("We have sliced %u times", l->slices);

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
