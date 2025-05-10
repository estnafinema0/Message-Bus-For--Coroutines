#include "corobus.h"

#include "libcoro.h"
#include "utils/rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct data_vector
{
	unsigned *data;
	size_t size;
	size_t capacity;
};

#if 1

/** Append @a count messages in @a data to the end of the vector. */
static void
data_vector_append_many(struct data_vector *vector,
						const unsigned *data, size_t count)
{
	if (vector->size + count > vector->capacity)
	{
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
	memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
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
struct wakeup_entry
{
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue
{
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

/** Instead of this function you can write this construction in the code
 *
 * struct wakeup_entry entry = {.coro = coro_this()};
 * rlist_add_tail(&chan->recv_queue.coros, &entry.base);
 * coro_suspend();
 * rlist_del(&entry.base);
 */

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

struct coro_bus_channel
{
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	struct data_vector data;
};

struct coro_bus
{
	struct coro_bus_channel **channels;
	int channel_count;
	struct wakeup_queue broadcast_queue;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *bus = malloc(sizeof(*bus));
	if (!bus)
		return NULL;

	bus->channels = NULL;
	bus->channel_count = 0;
	rlist_create(&bus->broadcast_queue.coros);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return bus;
}

void coro_bus_delete(struct coro_bus *bus)
{
	if (bus == NULL)
		return;

	/* 1) wake up all broadcast waiting coros */
	while (!rlist_empty(&bus->broadcast_queue.coros))
	{
		struct wakeup_entry *e = rlist_shift_entry(
			&bus->broadcast_queue.coros,
			struct wakeup_entry, base);
		coro_wakeup(e->coro);
	}

	/* 2) wake up all send/recv for all channels */
	for (int i = 0; i < bus->channel_count; ++i)
	{
		struct coro_bus_channel *chan = bus->channels[i];
		if (!chan)
			continue;

		while (!rlist_empty(&chan->send_queue.coros))
		{
			struct wakeup_entry *e = rlist_shift_entry(
				&chan->send_queue.coros,
				struct wakeup_entry, base);
			coro_wakeup(e->coro);
		}
		while (!rlist_empty(&chan->recv_queue.coros))
		{
			struct wakeup_entry *e = rlist_shift_entry(
				&chan->recv_queue.coros,
				struct wakeup_entry, base);
			coro_wakeup(e->coro);
		}

		free(chan->data.data);
		free(chan);
	}

	free(bus->channels);
	free(bus);
	coro_bus_errno_set(CORO_BUS_ERR_NONE);
}

int coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
	if (bus == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *chan = malloc(sizeof(*chan));
	if (chan == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NONE);
		return -1;
	}

	chan->size_limit = size_limit;
	rlist_create(&chan->recv_queue.coros);
	rlist_create(&chan->send_queue.coros);
	chan->data.data = NULL;
	chan->data.size = 0;
	chan->data.capacity = 0;

	int id = 0;
	for (id = 0; id < bus->channel_count; ++id)
	{
		if (bus->channels[id] == NULL)
		{
			bus->channels[id] = chan;
			break;
		}
	}

	if (id == bus->channel_count)
	{
		int new_count = bus->channel_count + 1;
		bus->channels = realloc(bus->channels, new_count * sizeof(*bus->channels));
		bus->channels[bus->channel_count] = chan;
		id = bus->channel_count;
		bus->channel_count = new_count;
	}

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return id;
}

void coro_bus_channel_close(struct coro_bus *bus, int channel)
{
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return;
	}

	struct coro_bus_channel *chan = bus->channels[channel];
	bus->channels[channel] = NULL;

	/* 1) Wake up all coroutines waiting for send with error NO_CHANNEL */
	while (!rlist_empty(&chan->send_queue.coros))
	{
		struct wakeup_entry *e = rlist_shift_entry(
			&chan->send_queue.coros,
			struct wakeup_entry, base);
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		coro_wakeup(e->coro);
	}
	/* 2) Wake up all coroutines waiting for recv with error NO_CHANNEL */
	while (!rlist_empty(&chan->recv_queue.coros))
	{
		struct wakeup_entry *e = rlist_shift_entry(
			&chan->recv_queue.coros,
			struct wakeup_entry, base);
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		coro_wakeup(e->coro);
	}
	/* 3) Wake up all coroutines waiting for broadcast with error NO_CHANNEL */
	while (!rlist_empty(&bus->broadcast_queue.coros))
	{
		struct wakeup_entry *e = rlist_shift_entry(
			&bus->broadcast_queue.coros,
			struct wakeup_entry, base);
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		coro_wakeup(e->coro);
	}

	free(chan->data.data);
	free(chan);
	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
}

int coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
	/*
	 * Try sending in a loop, until success. If error, then
	 * check which one is that. If 'wouldblock', then suspend
	 * this coroutine and try again when woken up.
	 *
	 * If see the channel has space, then wakeup the first
	 * coro in the send-queue. That is needed so when there is
	 * enough space for many messages, and many coroutines are
	 * waiting, they would then wake each other up one by one
	 * as lone as there is still space.
	 */
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *chan = bus->channels[channel];

	while (true)
	{
		if (coro_bus_try_send(bus, channel, data) == 0)
		{
			return 0;
		}
		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
		{
			return -1;
		}
		/* if  WOULD_BLOCK — block current corotine */
		wakeup_queue_suspend_this(&chan->send_queue);
	}
}

int coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *chan = bus->channels[channel];

	if (chan->data.size < chan->size_limit)
	{
		data_vector_append(&chan->data, data);
		coro_bus_errno_set(CORO_BUS_ERR_NONE);
		wakeup_queue_wakeup_first(&chan->recv_queue);
		return 0;
	}
	/*
	 * Append data if has space. Otherwise 'wouldblock' error.
	 * Wakeup the first coro in the recv-queue! To let it know
	 * there is data.
	 */
	coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
	return -1;
}

int coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *chan = bus->channels[channel];

	while (true)
	{
		if (coro_bus_try_recv(bus, channel, data) == 0)
		{
			return 0;
		}
		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
		{
			return -1;
		}
		/* if  WOULD_BLOCK — block current corotine */
		wakeup_queue_suspend_this(&chan->recv_queue);
	}
}

int coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *chan = bus->channels[channel];

	if (chan->data.size > 0)
	{
		unsigned int value = data_vector_pop_first(&chan->data);
		*data = value;
		coro_bus_errno_set(CORO_BUS_ERR_NONE);
		wakeup_queue_wakeup_first(&chan->send_queue);
		return 0;
	}

	coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
	return -1;
}

#if NEED_BROADCAST

int coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	if (!bus || bus->channel_count == 0)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	while (true)
	{
		if (coro_bus_try_broadcast(bus, data) == 0)
			return 0;
		if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
			return -1;
		wakeup_queue_suspend_this(&bus->broadcast_queue);
	}
}

int coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	if (!bus || bus->channel_count == 0)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	bool any = false;

	for (int id = 0; id < bus->channel_count; ++id)
	{
		struct coro_bus_channel *chan = bus->channels[id];
		if (!chan)
			continue;
		any = true;
		if (chan->data.size >= chan->size_limit)
		{
			coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
			return -1;
		}
	}

	if (!any)
	{
		/* If all channels were null */
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	for (int id = 0; id < bus->channel_count; ++id)
	{
		if (!bus->channels[id])
			continue;
		data_vector_append(&bus->channels[id]->data, data);
		wakeup_queue_wakeup_first(&bus->channels[id]->recv_queue);
	}

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	return 0;
}

#endif

#if NEED_BATCH

int coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *chan = bus->channels[channel];

	int sent = coro_bus_try_send_v(bus, channel, data, count);
	if (sent >= 0)
		return sent;
	if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
		return -1;

	/* if  WOULD_BLOCK — block current corotine */
	wakeup_queue_suspend_this(&chan->send_queue);

	/* After wake up */
	sent = coro_bus_try_send_v(bus, channel, data, count);
	if (sent < 0 && coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
		return -1;
	return sent;
}

int coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *chan = bus->channels[channel];

	/* If no space for even one message */
	if (chan->data.size >= chan->size_limit)
	{
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	/* How much can send actually */
	unsigned can = chan->size_limit - chan->data.size;
	unsigned to_send = (count < can ? count : can);

	data_vector_append_many(&chan->data, data, to_send);

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	wakeup_queue_wakeup_first(&chan->recv_queue);

	return to_send;
}

int coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	struct coro_bus_channel *chan = bus->channels[channel];

	int recv = coro_bus_try_recv_v(bus, channel, data, capacity);
	if (recv >= 0)
		return recv;
	if (coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
		return -1;

	/* if  WOULD_BLOCK — block current corotine */
	wakeup_queue_suspend_this(&chan->recv_queue);

	/* After wake up */
	recv = coro_bus_try_recv_v(bus, channel, data, capacity);
	if (recv < 0 && coro_bus_errno() == CORO_BUS_ERR_NO_CHANNEL)
		return -1;
	return recv;
}

int coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	if (!bus || channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *chan = bus->channels[channel];

	if (chan->data.size == 0)
	{
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	unsigned have = chan->data.size;
	unsigned to_recv = (capacity < have ? capacity : have);

	data_vector_pop_first_many(&chan->data, data, to_recv);

	coro_bus_errno_set(CORO_BUS_ERR_NONE);
	wakeup_queue_wakeup_first(&chan->send_queue);

	return to_recv;
}

#endif
