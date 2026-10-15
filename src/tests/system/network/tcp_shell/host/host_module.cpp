/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Builtin-only module manager for host builds.

	The kernel-land emulation's module.cpp cannot be built for the host: it
	loads modules as BeOS add-on images (load_add_on()/get_image_symbol()) and
	walks directories with BPath/BDirectory, none of which exists off Haiku.

	A host test does not need any of that. Everything under test is linked into
	the binary and announced with _add_builtin_module(), so this file implements
	just the builtin half of the same interface: a flat list of module_info*
	with Haiku's reference-counting and std_ops() semantics, which is what the
	TCP stack actually depends on.

	Module dependencies are resolved by referencing the executable's
	module_dependencies array directly (weakly, since not every host test
	defines one) instead of looking the symbol up in a loaded image.
*/

#include <module.h>
#include <kmodule.h>
#include <Notifications.h>

#include <pthread.h>
#include <set>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>


struct module_name_list {
	std::set<std::string>				names;
	std::set<std::string>::iterator		it;
};


struct builtin_module {
	module_info*	info;
	int32			referenceCount;
	bool			initialized;
};


static std::vector<builtin_module*> sModules;
static pthread_mutex_t sModuleLock = PTHREAD_MUTEX_INITIALIZER;

/*	Defined by whichever module source is linked in (tcp.cpp, for the TCP
	shell). Weak so that a host test without dependencies still links. */
extern module_dependency module_dependencies[] __attribute__((weak));


class ModuleLocker {
public:
	ModuleLocker()		{ pthread_mutex_lock(&sModuleLock); }
	~ModuleLocker()		{ pthread_mutex_unlock(&sModuleLock); }
};


static builtin_module*
find_builtin_module(const char* name)
{
	if (name == NULL)
		return NULL;

	for (size_t i = 0; i < sModules.size(); i++) {
		if (strcmp(sModules[i]->info->name, name) == 0)
			return sModules[i];
	}

	return NULL;
}


static status_t
get_builtin_module(builtin_module* module)
{
	if (module->referenceCount == 0 && !module->initialized) {
		if (module->info->std_ops != NULL) {
			status_t status = module->info->std_ops(B_MODULE_INIT);
			if (status < B_OK)
				return status;
		}
		module->initialized = true;
	}

	module->referenceCount++;
	return B_OK;
}


//	#pragma mark - private emulation functions


extern "C" status_t
_add_builtin_module(module_info* info)
{
	if (info == NULL)
		return B_BAD_VALUE;

	ModuleLocker _;

	builtin_module* module = new builtin_module;
	module->info = info;
	module->referenceCount = 0;
	module->initialized = false;
	sModules.push_back(module);

	return B_OK;
}


extern "C" status_t
_get_builtin_dependencies(void)
{
	if (module_dependencies == NULL)
		return B_OK;

	for (uint32 i = 0; module_dependencies[i].name != NULL; i++) {
		status_t status = get_module(module_dependencies[i].name,
			module_dependencies[i].info);
		if (status < B_OK)
			return status;
	}

	return B_OK;
}


//	#pragma mark - emulated kernel functions


status_t
get_module(const char* path, module_info** _info)
{
	if (path == NULL || _info == NULL)
		return B_BAD_VALUE;

	ModuleLocker _;

	builtin_module* module = find_builtin_module(path);
	if (module == NULL)
		return B_ENTRY_NOT_FOUND;

	status_t status = get_builtin_module(module);
	if (status < B_OK)
		return status;

	*_info = module->info;
	return B_OK;
}


status_t
put_module(const char* path)
{
	if (path == NULL)
		return B_BAD_VALUE;

	ModuleLocker _;

	builtin_module* module = find_builtin_module(path);
	if (module == NULL)
		return B_BAD_VALUE;

	if (--module->referenceCount > 0)
		return B_OK;

	// A builtin module is never unloaded, so -- unlike the add-on case -- the
	// module_info stays valid and the list entry stays put; only the init
	// state is given back.
	if (module->initialized && module->info->std_ops != NULL)
		module->info->std_ops(B_MODULE_UNINIT);
	module->initialized = false;

	return B_OK;
}


status_t
get_next_loaded_module_name(uint32* cookie, char* name, size_t* nameLength)
{
	if (cookie == NULL || name == NULL || nameLength == NULL)
		return B_BAD_VALUE;

	ModuleLocker _;

	if (*cookie >= sModules.size())
		return B_ENTRY_NOT_FOUND;

	const char* moduleName = sModules[*cookie]->info->name;
	size_t length = strlen(moduleName);
	if (length >= *nameLength)
		return B_BAD_VALUE;

	strcpy(name, moduleName);
	*nameLength = length;
	(*cookie)++;

	return B_OK;
}


static bool
matches_suffix(const char* name, const char* suffix)
{
	if (suffix == NULL || suffix[0] == '\0')
		return true;

	size_t nameLength = strlen(name);
	size_t suffixLength = strlen(suffix);
	if (nameLength <= suffixLength || name[nameLength - suffixLength - 1] != '/')
		return false;

	return strcmp(name + nameLength - suffixLength, suffix) == 0;
}


void*
open_module_list_etc(const char* prefix, const char* suffix)
{
	if (prefix == NULL)
		return NULL;

	ModuleLocker _;

	module_name_list* list = new module_name_list;
	size_t prefixLength = strlen(prefix);

	for (size_t i = 0; i < sModules.size(); i++) {
		const char* name = sModules[i]->info->name;
		if (strncmp(name, prefix, prefixLength) == 0
			&& matches_suffix(name, suffix))
			list->names.insert(name);
	}

	list->it = list->names.begin();
	return list;
}


void*
open_module_list(const char* prefix)
{
	return open_module_list_etc(prefix, NULL);
}


status_t
read_next_module_name(void* cookie, char* buffer, size_t* bufferSize)
{
	module_name_list* list = (module_name_list*)cookie;
	if (list == NULL || buffer == NULL || bufferSize == NULL)
		return B_BAD_VALUE;

	if (list->it == list->names.end())
		return B_ENTRY_NOT_FOUND;

	const std::string& name = *list->it;
	if (name.length() >= *bufferSize)
		return B_BAD_VALUE;

	strcpy(buffer, name.c_str());
	*bufferSize = name.length();
	list->it++;

	return B_OK;
}


status_t
close_module_list(void* cookie)
{
	module_name_list* list = (module_name_list*)cookie;
	if (list == NULL)
		return B_BAD_VALUE;

	delete list;
	return B_OK;
}


status_t
start_watching_modules(const char* prefix, NotificationListener& listener)
{
	return B_NOT_SUPPORTED;
}


status_t
stop_watching_modules(const char* prefix, NotificationListener& listener)
{
	return B_NOT_SUPPORTED;
}
