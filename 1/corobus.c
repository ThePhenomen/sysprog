#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

struct data_vector {
	unsigned *data;
	size_t size;
	size_t capacity;
};

#if 0

/** Append @a count messages in @a data to the end of the vector. */
static void
data_vector_append_many(struct data_vector *vector,
	const unsigned *data, size_t count)
{
	if (vector->size + count > vector->capacity) {
		if (vector->capacity == 0)
			vector->capacity = 4;
		else
			vector->capacity *= 2;
		if (vector->capacity < vector->size + count)
			vector->capacity = vector->size + count;
		vector->data = realloc(vector->data,
			sizeof(vector->data[0]) * vector->capacity);
	}
	memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
	vector->size += count;
}

/** Append a single message to the vector. */
static void
data_vector_append(struct data_vector *vector, unsigned data)
{
	data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void
data_vector_pop_first_many(struct data_vector *vector, unsigned *data, size_t count)
{
	assert(count <= vector->size);
	memcpy(data, vector->data, sizeof(data[0]) * count);
	vector->size -= count;
	if (vector->size > 0) {
		memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
	}
}

/** Pop a single message from the head of the vector. */
static unsigned
data_vector_pop_first(struct data_vector *vector)
{
	unsigned data = 0;
	data_vector_pop_first_many(vector, &data, 1);
	return data;
}

#endif

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
	struct rlist coros;
};

#if 1

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	coro_wakeup(entry->coro);
}

#endif

struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	struct data_vector data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
	int max_channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *bus = malloc(sizeof(*bus));
	if (!bus) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return NULL;
    	};

	bus->channels = NULL;
	bus->channel_count = 0;
		bus->max_channel_count = 0;

    	return bus;
}

void
coro_bus_delete(struct coro_bus *bus)
{
	if (!bus) 
		return;

    	for (int i = 0; i < bus->max_channel_count; i++) {
		if (!bus->channels[i]) 
			continue;
		
	        free(bus->channels[i]->data.data);
		free(bus->channels[i]);
	};
	
	free(bus->channels);
	free(bus);
}

int 
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit) 
{
	if (!bus || size_limit == 0) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
    	};

    	struct coro_bus_channel *channel = calloc(1, sizeof(*channel));
    	if (!channel) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
    	};

    	channel->data.data = calloc(size_limit, sizeof(unsigned));
    	if (!channel->data.data) {
        	free(channel);
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
    	};

    	channel->size_limit = size_limit;
    	channel->data.size = 0;
    	channel->data.capacity = size_limit;
    	rlist_create(&channel->send_queue.coros);
    	rlist_create(&channel->recv_queue.coros);

    	struct coro_bus_channel **new_channels = realloc(bus->channels, (bus->channel_count + 1) * sizeof(*bus->channels));
    	if (!new_channels) {
        	free(channel->data.data);
        	free(channel);
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
    	};

    	bus->channels = new_channels;
    	bus->channels[bus->channel_count] = channel;
	bus->max_channel_count++;

    	return bus->channel_count++;
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel) 
{
	if (!bus || channel < 0 || !bus->channels) 
		return;

    	struct coro_bus_channel *ch = bus->channels[channel];

    	while (!rlist_empty(&ch->send_queue.coros)) {
		struct wakeup_entry *entry = rlist_first_entry(&ch->send_queue.coros, struct wakeup_entry, base);
		rlist_del(&entry->base);
		coro_wakeup(entry->coro);
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
	};

    	while (!rlist_empty(&ch->recv_queue.coros)) {
		struct wakeup_entry *entry = rlist_first_entry(&ch->recv_queue.coros, struct wakeup_entry, base);
		rlist_del(&entry->base);
		coro_wakeup(entry->coro);
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
    	};

    	free(ch->data.data);
    	free(ch);

    	bus->channel_count--;

    	if (bus->channel_count == 0) {
        	free(bus->channels);
        	bus->channels = NULL;
		bus->max_channel_count=0;
    	} 
	else {
        	bus->channels = realloc(bus->channels, bus->channel_count * sizeof(*bus->channels));
    	};

}

int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{	
	if (!bus || channel < 0  || !bus->channels) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
    	};

    	struct coro_bus_channel *ch = bus->channels[channel];

    	while (ch->data.size >= ch->size_limit) {
        	wakeup_queue_suspend_this(&ch->send_queue);
		
		if (!bus->channels) {
            		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            		return -1;
        	};

	};

    	ch->data.data[ch->data.size++] = data;

    	if (!rlist_empty(&ch->recv_queue.coros)) 
		wakeup_queue_wakeup_first(&ch->recv_queue);

    	return 0;
}

int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
	if (!bus || channel < 0 || !bus->channels) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
   	};

    	struct coro_bus_channel *ch = bus->channels[channel];

    	if (ch->data.size >= ch->size_limit) {
        	coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
        	return -1;
    	};

    	ch->data.data[ch->data.size++] = data;

	if (!rlist_empty(&ch->recv_queue.coros))
    		wakeup_queue_wakeup_first(&ch->recv_queue);

    	return 0;
}

int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{	

	if (!bus || channel < 0 || !bus->channels) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
    	};

    	struct coro_bus_channel *ch = bus->channels[channel];

    	while (ch->data.size == 0) {
       		wakeup_queue_suspend_this(&ch->recv_queue);
		
		if (!bus->channels) {
            		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
            		return -1;
        	};

    	};

    	*data = ch->data.data[0];
	if (ch->data.size > 1) {
    		memmove(&ch->data.data[0], &ch->data.data[1], (ch->data.size - 1) * sizeof(unsigned));
	};
	ch->data.size--;

    	if (!rlist_empty(&ch->send_queue.coros))
        	wakeup_queue_wakeup_first(&ch->send_queue);

    	return 0;
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data) 
{
	
	if (!bus || channel < 0 || !bus->channels) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
   	};

   	struct coro_bus_channel *ch = bus->channels[channel];

    	if (ch->data.size == 0) {
        	coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
	    	return -1;
    	};

	*data = ch->data.data[0];
	if (ch->data.size > 1) {
    		memmove(&ch->data.data[0], &ch->data.data[1], (ch->data.size - 1) * sizeof(unsigned));
	};
	ch->data.size--;

	if (!rlist_empty(&ch->send_queue.coros))
        	wakeup_queue_wakeup_first(&ch->send_queue);

    	return 0;
}


#if NEED_BROADCAST

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	if (!bus || bus->max_channel_count == 0 || !bus->channels) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
    	};

	int sent = 0;

    	while (1) {
        	int can_send_all = 1;

        	for (int i = 0; i < bus->max_channel_count; i++) {
            		struct coro_bus_channel *ch = bus->channels[i];
		        if (!ch) 
				continue; 

            		if (ch->data.size >= ch->size_limit) {
                		can_send_all = 0;
                		wakeup_queue_suspend_this(&ch->send_queue);

                		if (i >= bus->max_channel_count || !bus->channels[i]) {
                    			coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
                    			return -1;
                		};

            		};

        	};

        	if (can_send_all) 
			break; 
    	};

    	for (int i = 0; i < bus->max_channel_count; i++) {
        	struct coro_bus_channel *ch = bus->channels[i];
       	 	if (!ch) 
			continue;

        	ch->data.data[ch->data.size++] = data;
        	sent++;

        	if (!rlist_empty(&ch->recv_queue.coros))
        		wakeup_queue_wakeup_first(&ch->recv_queue);
    	};

    	return sent > 0 ? 0 : -1;
}

int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	if (!bus || bus->max_channel_count == 0 || !bus->channels) {
        	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        	return -1;
    	};

	int sent = 0;

    	for (int i = 0; i < bus->max_channel_count; i++) {
        	if (!bus->channels[i]) 
			continue;

        	if (bus->channels[i]->data.size >= bus->channels[i]->size_limit) {
            		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
            		return -1;
        	};

    	};

    	for (int i = 0; i < bus->max_channel_count; i++) {
        	struct coro_bus_channel *ch = bus->channels[i];
        	if (!ch) 
			continue;

        	ch->data.data[ch->data.size++] = data;
        	sent++;

        	if (!rlist_empty(&ch->recv_queue.coros))
            		wakeup_queue_wakeup_first(&ch->recv_queue);
    	};

    	return sent > 0 ? 0 : -1;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)count;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)count;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)capacity;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

int
coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	/* IMPLEMENT THIS FUNCTION */
	(void)bus;
	(void)channel;
	(void)data;
	(void)capacity;
	coro_bus_errno_set(CORO_BUS_ERR_NOT_IMPLEMENTED);
	return -1;
}

#endif
