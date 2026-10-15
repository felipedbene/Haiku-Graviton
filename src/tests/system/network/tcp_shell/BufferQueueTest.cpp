/*
 * Copyright 2009, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */

#include "BufferQueue.h"

#include <stdio.h>
#include <string.h>


extern "C" status_t _add_builtin_module(module_info *info);

extern struct net_buffer_module_info gNetBufferModule;
	// from net_buffer.cpp

struct net_socket_module_info gNetSocketModule;
struct net_buffer_module_info* gBufferModule;

BufferQueue gQueue(32768);


net_buffer*
create_filled_buffer(size_t bytes)
{
	const static uint8 data[4096] = {0};

	net_buffer* buffer = gBufferModule->create(256);
	if (buffer == NULL) {
		printf("creating a buffer failed!\n");
		return NULL;
	}

	status_t status = gBufferModule->append(buffer, data, bytes);
	if (status != B_OK) {
		printf("appending %" B_PRIuSIZE " bytes to buffer %p failed: %s\n",
			bytes, buffer, strerror(status));
		gBufferModule->free(buffer);
		return NULL;
	}

	return buffer;
}


void
add(size_t bytes, uint32 at)
{
	gQueue.Add(create_filled_buffer(bytes), at);
}


void
eat(size_t bytes)
{
	net_buffer* buffer = NULL;
	status_t status = gQueue.Get(bytes, true, &buffer);
	if (status == B_OK) {
		ASSERT(buffer->size == bytes);
		gBufferModule->free(buffer);
	} else {
		printf("getting %" B_PRIuSIZE " bytes failed: %s\n", bytes,
			strerror(status));
	}
}


void
dump(const char* text = "")
{
	// tcp_sequence holds its value privately and has no conversion operator,
	// so Number() is the only way out of it.
	printf("%s (available %" B_PRIuSIZE " at %" B_PRIu32 ")\n", text,
		gQueue.Available(), gQueue.FirstSequence().Number());

	// Dump() and the Verify() behind BufferQueue's internal VERIFY() only exist
	// when the tree's DEBUG_TCP_BUFFER_QUEUE is on (it follows KDEBUG_LEVEL_2,
	// so on by default). Predefining it here would not help -- BufferQueue.cpp
	// is a separate translation unit and takes the config's value -- so follow
	// the config instead of fighting it.
#if DEBUG_TCP_BUFFER_QUEUE
	gQueue.Dump();
#endif
}


int
main()
{
	_add_builtin_module((module_info*)&gNetBufferModule);
	get_module(NET_BUFFER_MODULE_NAME, (module_info**)&gBufferModule);
	gQueue.SetInitialSequence(100);

	add(100, 100);
	add(100, 300);
	add(100, 250);
	add(100, 175);
	ASSERT(gQueue.Available() == 300);
	dump("add 4");

	eat(99);
	dump("ate 99");

	eat(1);
	eat(1);
	eat(149);
	eat(50);

	add(10, 100);
	add(0, 400);
	add(1, 399);
	dump("add nothing");

	add(1, 1000);
	dump("add far away");

	add(2, 399);
	dump("add 1");

	add(100, 500);
	add(10, 480);
	add(19, 401);
	add(10, 460);
	add(10, 420);
	add(30, 430);
	add(35, 465);
	dump("added with holes");

	add(50, 425);
	dump("added no new data");

	eat(19);
	eat(1);
	eat(40);
	eat(50);
	dump("ate some");

	add(1, 999);
	dump("add 1");
	add(2, 999);
	add(2, 999);
	dump("add 2");
	add(3, 999);
	dump("add 3");

	add(60, 540);
	dump("added at the end of previous data");
	
	add(998, 1002);
	add(500, 1000);
	dump("added data covered by next");

	put_module(NET_BUFFER_MODULE_NAME);
	return 0;
}
