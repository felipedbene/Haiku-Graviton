/*
 * Copyright 2026, DeBeOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * fdescfs, the file system mounted at /dev/fd. Its root lists the file
 * descriptors of the team that reads it, and /dev/fd/<n> stands for that
 * team's descriptor <n>. Opening /dev/fd/<n> does not open a node: the VFS
 * hands the open to fdescfs_open_vnode(), which duplicates descriptor <n>
 * the way dup() does. That is the BSDs' fdescfs behaviour, and it is what
 * makes /dev/fd/0 read whatever stdin is -- a pipe or a socket as well as a
 * file -- where re-opening a path could not. devfs links /dev/stdin,
 * /dev/stdout and /dev/stderr to fd/0, fd/1 and fd/2.
 *
 * The nodes carry no state: node kFirstDescriptorID + <n> means descriptor
 * <n> of whichever team asks, so nothing has to be created or destroyed as
 * teams open and close descriptors.
 */


#include <fs/fdescfs.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fs_info.h>
#include <limits.h>
#include <new>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <AutoDeleterDrivers.h>
#include <KernelExport.h>
#include <util/AutoLock.h>
#include <vfs.h>

#include <fs/fd.h>


//#define TRACE_FDESCFS
#ifdef TRACE_FDESCFS
#	define TRACE(x) dprintf x
#else
#	define TRACE(x)
#endif


namespace {


static const ino_t kRootID = 1;
static const ino_t kFirstDescriptorID = 2;


struct fdesc_node {
	int		fd;
		// -1 for the root directory
};

struct fdesc_volume {
	timespec	mount_time;
};

struct dir_cookie {
	uint32	next;
		// 0 is ".", 1 is "..", and n >= 2 resumes at descriptor n - 2
};


extern fs_volume_ops sVolumeOps;
extern fs_vnode_ops sRootOps;
extern fs_vnode_ops sDescriptorOps;


/*!	Parses a /dev/fd entry name, i.e. a descriptor number written in decimal
	without leading zeros.
*/
static bool
parse_descriptor(const char* name, int& _fd)
{
	if (name[0] < '0' || name[0] > '9' || (name[0] == '0' && name[1] != '\0'))
		return false;

	int64 fd = 0;
	for (const char* c = name; *c != '\0'; c++) {
		if (*c < '0' || *c > '9')
			return false;
		fd = fd * 10 + *c - '0';
		if (fd > INT_MAX)
			return false;
	}

	_fd = fd;
	return true;
}


/*!	The descriptors this file system shows are those of the calling team. */
static file_descriptor*
get_caller_descriptor(int fd)
{
	return get_fd(get_current_io_context(false), fd);
}


static void
fill_dirent(fs_volume* volume, struct dirent* dirent, ino_t id,
	const char* name)
{
	dirent->d_dev = volume->id;
	dirent->d_ino = id;
	dirent->d_reclen = offsetof(struct dirent, d_name) + strlen(name) + 1;
}


//	#pragma mark - volume


static status_t
fdescfs_mount(fs_volume* volume, const char* device, uint32 flags,
	const char* args, ino_t* _rootID)
{
	fdesc_volume* fdescVolume = new(std::nothrow) fdesc_volume;
	fdesc_node* root = new(std::nothrow) fdesc_node;
	if (fdescVolume == NULL || root == NULL) {
		delete fdescVolume;
		delete root;
		return B_NO_MEMORY;
	}

	bigtime_t now = real_time_clock_usecs();
	fdescVolume->mount_time.tv_sec = now / 1000000;
	fdescVolume->mount_time.tv_nsec = (now % 1000000) * 1000;
	root->fd = -1;

	volume->private_volume = fdescVolume;
	volume->ops = &sVolumeOps;

	status_t status = publish_vnode(volume, kRootID, root, &sRootOps, S_IFDIR,
		0);
	if (status != B_OK) {
		delete root;
		delete fdescVolume;
		return status;
	}

	*_rootID = kRootID;
	return B_OK;
}


static status_t
fdescfs_unmount(fs_volume* volume)
{
	delete (fdesc_volume*)volume->private_volume;
	return B_OK;
}


static status_t
fdescfs_read_fs_info(fs_volume* volume, struct fs_info* info)
{
	info->flags = 0;
	info->block_size = 0;
	info->io_size = 0;
	info->total_blocks = 0;
	info->free_blocks = 0;
	info->total_nodes = 0;
	info->free_nodes = 0;
	strlcpy(info->volume_name, "fd", sizeof(info->volume_name));
	return B_OK;
}


static status_t
fdescfs_get_vnode(fs_volume* volume, ino_t id, fs_vnode* vnode, int* _type,
	uint32* _flags, bool reenter)
{
	if (id < kFirstDescriptorID || id - kFirstDescriptorID > INT_MAX)
		return B_ENTRY_NOT_FOUND;
			// the root is published at mount time and never released

	fdesc_node* node = new(std::nothrow) fdesc_node;
	if (node == NULL)
		return B_NO_MEMORY;

	node->fd = id - kFirstDescriptorID;

	vnode->private_node = node;
	vnode->ops = &sDescriptorOps;
	*_type = S_IFCHR;
	*_flags = 0;
	return B_OK;
}


//	#pragma mark - nodes


static status_t
fdescfs_lookup(fs_volume* volume, fs_vnode* dir, const char* name,
	ino_t* _id)
{
	TRACE(("fdescfs_lookup(\"%s\")\n", name));

	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		// The VFS resolves ".." of a mount root in the covered directory.
		*_id = kRootID;
		return get_vnode(volume, kRootID, NULL);
	}

	int fd;
	if (!parse_descriptor(name, fd))
		return B_ENTRY_NOT_FOUND;

	FileDescriptorPutter descriptor(get_caller_descriptor(fd));
	if (!descriptor.IsSet())
		return B_ENTRY_NOT_FOUND;

	*_id = kFirstDescriptorID + fd;
	return get_vnode(volume, *_id, NULL);
}


static status_t
fdescfs_descriptor_lookup(fs_volume* volume, fs_vnode* dir, const char* name,
	ino_t* _id)
{
	return B_NOT_A_DIRECTORY;
}


static status_t
fdescfs_get_vnode_name(fs_volume* volume, fs_vnode* vnode, char* buffer,
	size_t bufferSize)
{
	fdesc_node* node = (fdesc_node*)vnode->private_node;
	if (node->fd < 0)
		return B_BAD_VALUE;

	if ((size_t)snprintf(buffer, bufferSize, "%d", node->fd) >= bufferSize)
		return B_BUFFER_OVERFLOW;

	return B_OK;
}


static status_t
fdescfs_put_vnode(fs_volume* volume, fs_vnode* vnode, bool reenter)
{
	delete (fdesc_node*)vnode->private_node;
	return B_OK;
}


static status_t
fdescfs_access(fs_volume* volume, fs_vnode* vnode, int mode)
{
	fdesc_node* node = (fdesc_node*)vnode->private_node;
	if (node->fd < 0 && (mode & W_OK) != 0)
		return B_READ_ONLY_DEVICE;

	// What a descriptor node may be opened for is decided by the descriptor
	// it duplicates, see open_descriptor().
	return B_OK;
}


static status_t
fdescfs_read_stat(fs_volume* volume, fs_vnode* vnode, struct stat* stat)
{
	fdesc_node* node = (fdesc_node*)vnode->private_node;

	if (node->fd >= 0) {
		// Report what the descriptor refers to, as fstat() on it would.
		FileDescriptorPutter descriptor(get_caller_descriptor(node->fd));
		if (!descriptor.IsSet())
			return B_ENTRY_NOT_FOUND;

		if (descriptor->ops->fd_read_stat != NULL)
			return descriptor->ops->fd_read_stat(descriptor.Get(), stat);
	}

	// The root, or a descriptor without a stat of its own (an event queue,
	// say).
	const timespec& time
		= ((fdesc_volume*)volume->private_volume)->mount_time;

	memset(stat, 0, sizeof(*stat));
	stat->st_mode = node->fd < 0 ? S_IFDIR | 0555 : S_IFCHR | 0600;
	stat->st_nlink = node->fd < 0 ? 2 : 1;
	stat->st_blksize = 512;
	stat->st_atim = stat->st_mtim = stat->st_ctim = stat->st_crtim = time;
	return B_OK;
}


static status_t
fdescfs_open(fs_volume* volume, fs_vnode* vnode, int openMode, void** _cookie)
{
	fdesc_node* node = (fdesc_node*)vnode->private_node;

	// A descriptor node is never opened as such, see fdescfs_open_vnode().
	if (node->fd >= 0 || (openMode & O_RWMASK) != O_RDONLY)
		return B_NOT_ALLOWED;

	*_cookie = NULL;
	return B_OK;
}


static status_t
fdescfs_close(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	return B_OK;
}


static status_t
fdescfs_free_cookie(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	return B_OK;
}


static status_t
fdescfs_read(fs_volume* volume, fs_vnode* vnode, void* cookie, off_t pos,
	void* buffer, size_t* _length)
{
	*_length = 0;
	return B_IS_A_DIRECTORY;
}


static status_t
fdescfs_open_dir(fs_volume* volume, fs_vnode* vnode, void** _cookie)
{
	dir_cookie* cookie = new(std::nothrow) dir_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->next = 0;
	*_cookie = cookie;
	return B_OK;
}


static status_t
fdescfs_close_dir(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	return B_OK;
}


static status_t
fdescfs_free_dir_cookie(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	delete (dir_cookie*)cookie;
	return B_OK;
}


static status_t
fdescfs_read_dir(fs_volume* volume, fs_vnode* vnode, void* _cookie,
	struct dirent* dirent, size_t bufferSize, uint32* _num)
{
	dir_cookie* cookie = (dir_cookie*)_cookie;
	char name[16];
	ino_t id;
	uint32 next;

	if (cookie->next < 2) {
		strcpy(name, cookie->next == 0 ? "." : "..");
		id = kRootID;
		next = cookie->next + 1;
	} else {
		// find the caller's next open descriptor
		io_context* context = get_current_io_context(false);
		ReadLocker locker(context->lock);

		uint32 fd = cookie->next - 2;
		while (fd < context->table_size && context->fds[fd] == NULL)
			fd++;
		if (fd >= context->table_size) {
			*_num = 0;
			return B_OK;
		}

		snprintf(name, sizeof(name), "%" B_PRIu32, fd);
		id = kFirstDescriptorID + fd;
		next = fd + 3;
	}

	fill_dirent(volume, dirent, id, name);
	if (dirent->d_reclen > bufferSize)
		return B_BUFFER_OVERFLOW;

	ssize_t length = user_strlcpy(dirent->d_name, name,
		bufferSize - offsetof(struct dirent, d_name));
	if (length < B_OK)
		return length;

	cookie->next = next;
	*_num = 1;
	return B_OK;
}


static status_t
fdescfs_rewind_dir(fs_volume* volume, fs_vnode* vnode, void* cookie)
{
	((dir_cookie*)cookie)->next = 0;
	return B_OK;
}


/*!	Opens descriptor node \a fd: duplicates the caller's descriptor \a fd. */
static int
open_descriptor(int fd, int openMode, bool kernel)
{
	io_context* context = get_current_io_context(kernel);

	file_descriptor* descriptor = get_fd(context, fd);
	if (descriptor == NULL)
		return B_FILE_ERROR;

	// The duplicate shares the open file description, so it cannot be had
	// for more access than the descriptor itself has. Like dup(), it ignores
	// the other open flags (O_TRUNC, O_APPEND, ...), as the BSDs do.
	int accessMode = openMode & O_RWMASK;
	int allowedMode = descriptor->open_mode & O_RWMASK;
	if ((accessMode != O_WRONLY && allowedMode == O_WRONLY)
		|| (accessMode != O_RDONLY && allowedMode == O_RDONLY)) {
		put_fd(descriptor);
		return B_PERMISSION_DENIED;
	}

	int newFD = new_fd(context, descriptor);
	if (newFD < 0) {
		put_fd(descriptor);
		return newFD;
	}

	WriteLocker locker(context->lock);
	fd_set_close_on_exec(context, newFD, (openMode & O_CLOEXEC) != 0);
	fd_set_close_on_fork(context, newFD, (openMode & O_CLOFORK) != 0);

	return newFD;
}


fs_volume_ops sVolumeOps = {
	&fdescfs_unmount,
	&fdescfs_read_fs_info,
	NULL,	// write_fs_info
	NULL,	// sync
	&fdescfs_get_vnode,
};


fs_vnode_ops sRootOps = {
	&fdescfs_lookup,
	NULL,	// get_vnode_name: the VFS uses the covered vnode's name
	&fdescfs_put_vnode,
	NULL,	// remove_vnode

	NULL,	// can_page
	NULL,	// read_pages
	NULL,	// write_pages

	NULL,	// io
	NULL,	// cancel_io

	NULL,	// get_file_map

	NULL,	// ioctl
	NULL,	// set_flags
	NULL,	// select
	NULL,	// deselect
	NULL,	// fsync

	NULL,	// read_symlink
	NULL,	// create_symlink

	NULL,	// link
	NULL,	// unlink
	NULL,	// rename

	&fdescfs_access,
	&fdescfs_read_stat,
	NULL,	// write_stat
	NULL,	// preallocate

	NULL,	// create
	&fdescfs_open,
	&fdescfs_close,
	&fdescfs_free_cookie,
	&fdescfs_read,
	NULL,	// write

	NULL,	// create_dir
	NULL,	// remove_dir
	&fdescfs_open_dir,
	&fdescfs_close_dir,
	&fdescfs_free_dir_cookie,
	&fdescfs_read_dir,
	&fdescfs_rewind_dir,
};


fs_vnode_ops sDescriptorOps = {
	&fdescfs_descriptor_lookup,
	&fdescfs_get_vnode_name,
	&fdescfs_put_vnode,
	NULL,	// remove_vnode

	NULL,	// can_page
	NULL,	// read_pages
	NULL,	// write_pages

	NULL,	// io
	NULL,	// cancel_io

	NULL,	// get_file_map

	NULL,	// ioctl
	NULL,	// set_flags
	NULL,	// select
	NULL,	// deselect
	NULL,	// fsync

	NULL,	// read_symlink
	NULL,	// create_symlink

	NULL,	// link
	NULL,	// unlink
	NULL,	// rename

	&fdescfs_access,
	&fdescfs_read_stat,
	NULL,	// write_stat
	NULL,	// preallocate

	NULL,	// create
	&fdescfs_open,
	&fdescfs_close,
	&fdescfs_free_cookie,
	&fdescfs_read,
	NULL,	// write
};


static status_t
fdescfs_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;

		default:
			return B_ERROR;
	}
}


}	// namespace


/*!	If \a vnode is a /dev/fd/<n> node, "opens" it by duplicating the calling
	team's descriptor <n>, returns \c true, and sets \a _fd to the new
	descriptor or to an error. Returns \c false for any other node, which the
	VFS then opens the normal way.
*/
bool
fdescfs_open_vnode(fs_vnode* vnode, int openMode, bool kernel, int& _fd)
{
	if (vnode->ops != &sDescriptorOps)
		return false;

	_fd = open_descriptor(((fdesc_node*)vnode->private_node)->fd, openMode,
		kernel);
	return true;
}


file_system_module_info gFdescFileSystem = {
	{
		"file_systems/" FDESCFS_NAME B_CURRENT_FS_API_VERSION,
		0,
		fdescfs_std_ops,
	},

	FDESCFS_NAME,				// short_name
	"File Descriptor File System",	// pretty_name
	0,							// DDM flags

	NULL,	// identify_partition()
	NULL,	// scan_partition()
	NULL,	// free_identify_partition_cookie()
	NULL,	// free_partition_content_cookie()

	&fdescfs_mount,
};
