/* 
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <stdlib.h>
#include <unistd.h>
#include <syscalls.h>


void
_exit(int status)
{
	// Unlike exit(), this must not flush stdio (POSIX: "Open streams shall
	// not be flushed"). Its typical caller is a fork() child whose exec
	// failed: its stdio buffers are copies of its parent's, and flushing
	// them would write the parent's pending output a second time. The
	// kernel closes the team's file descriptors when it goes away.
	_kern_exit_team(status);
}


void
_Exit(int status)
{
	_exit(status);
}
