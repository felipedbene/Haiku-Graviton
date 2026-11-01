/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_FS_FDESCFS_H
#define _KERNEL_FS_FDESCFS_H


#include <fs_interface.h>


#define FDESCFS_NAME	"fdescfs"


bool	fdescfs_open_vnode(fs_vnode* vnode, int openMode, bool kernel,
			int& _fd);


#endif	// _KERNEL_FS_FDESCFS_H
