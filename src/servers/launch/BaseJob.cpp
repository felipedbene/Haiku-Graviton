/*
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#define _DEFAULT_SOURCE
#include "BaseJob.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <AutoDeleter.h>
#include <Message.h>
#include <OS.h>

#include "Conditions.h"
#include "Events.h"


BaseJob::BaseJob(const char* name)
	:
	BJob(name),
	fCondition(NULL),
	fEvent(NULL)
{
}


BaseJob::~BaseJob()
{
	delete fCondition;
}


const char*
BaseJob::Name() const
{
	return Title().String();
}


const ::Condition*
BaseJob::Condition() const
{
	return fCondition;
}


::Condition*
BaseJob::Condition()
{
	return fCondition;
}


void
BaseJob::SetCondition(::Condition* condition)
{
	fCondition = condition;
}


bool
BaseJob::CheckCondition(ConditionContext& context) const
{
	if (fCondition != NULL)
		return fCondition->Test(context);

	return true;
}


const ::Event*
BaseJob::Event() const
{
	return fEvent;
}


::Event*
BaseJob::Event()
{
	return fEvent;
}


void
BaseJob::SetEvent(::Event* event)
{
	fEvent = event;
	if (event != NULL)
		event->SetOwner(this);
}


/*!	Determines whether the events of this job has been triggered
	already or not.
	Note, if this job does not have any events, this method returns
	\c true.
*/
bool
BaseJob::EventHasTriggered() const
{
	return Event() == NULL || Event()->Triggered();
}


const BStringList&
BaseJob::Environment() const
{
	return fEnvironment;
}


BStringList&
BaseJob::Environment()
{
	return fEnvironment;
}


const BStringList&
BaseJob::EnvironmentSourceFiles() const
{
	return fSourceFiles;
}


BStringList&
BaseJob::EnvironmentSourceFiles()
{
	return fSourceFiles;
}


void
BaseJob::SetEnvironment(const BMessage& message)
{
	char* name;
	type_code type;
	int32 count;
	for (int32 index = 0; message.GetInfo(B_STRING_TYPE, index, &name, &type,
			&count) == B_OK; index++) {
		if (strcmp(name, "from_script") == 0) {
			const char* fromScript;
			for (int32 scriptIndex = 0; message.FindString(name, scriptIndex,
					&fromScript) == B_OK; scriptIndex++) {
				fSourceFiles.Add(fromScript);
			}
			continue;
		}

		BString variable = name;
		variable << "=";

		const char* argument;
		for (int32 argumentIndex = 0; message.FindString(name, argumentIndex,
				&argument) == B_OK; argumentIndex++) {
			if (argumentIndex > 0)
				variable << " ";
			variable += argument;
		}

		fEnvironment.Add(variable);
	}
}


void
BaseJob::GetSourceFilesEnvironment(BStringList& environment)
{
	int32 count = fSourceFiles.CountStrings();
	for (int32 index = 0; index < count; index++) {
		_GetSourceFileEnvironment(fSourceFiles.StringAt(index), environment);
	}
}


/*!	Gets the environment by evaluating the source files, and move that
	environment to the static environment.

	When this method returns, the source files list will be empty.
*/
void
BaseJob::ResolveSourceFiles()
{
	if (fSourceFiles.IsEmpty())
		return;

	GetSourceFilesEnvironment(fEnvironment);
	fSourceFiles.MakeEmpty();
}


void
BaseJob::_GetSourceFileEnvironment(const char* script, BStringList& environment)
{
	int pipes[2];
	if (pipe(&pipes[0]) != 0) {
		debug_printf("launch_daemon: env script \"%s\": could not create pipe: "
			"%s\n", script, strerror(errno));
		return;
	}

	posix_spawn_file_actions_t fileActions;
	int status = posix_spawn_file_actions_init(&fileActions);
	if (status != 0) {
		debug_printf("launch_daemon: env script \"%s\": could not init file "
			"actions: %s\n", script, strerror(status));
		close(pipes[0]);
		close(pipes[1]);
		return;
	}
	CObjectDeleter<posix_spawn_file_actions_t, int, posix_spawn_file_actions_destroy>
		actionsDeleter(&fileActions);

	// redirect stdout in the child
	posix_spawn_file_actions_addclose(&fileActions, STDOUT_FILENO);
	posix_spawn_file_actions_addclose(&fileActions, STDERR_FILENO);
	posix_spawn_file_actions_adddup2(&fileActions, pipes[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fileActions, pipes[1], STDERR_FILENO);

	for (int32 i = 0; i < 2; i++)
		posix_spawn_file_actions_addclose(&fileActions, pipes[i]);

	BString command;
	command.SetToFormat(". \"%s\"; export -p", script);

	const char* argv[] = {"/bin/sh", "-c", command.String(), NULL};

	pid_t child;
	status = posix_spawn(&child, argv[0], &fileActions, NULL, (char**)argv, NULL);

	if (status != 0) {
		debug_printf("launch_daemon: env script \"%s\": could not spawn "
			"helper shell: %s\n", script, strerror(status));
		close(pipes[0]);
		close(pipes[1]);
		return;
	}

	// Retrieve environment from child

	close(pipes[1]);

	// A from_script helper is untrusted from launch_daemon's point of view: it
	// may hang (a slow/blocking script early in boot) or produce unbounded
	// output. An unbounded, blocking read here would wedge the job forever,
	// leaving it enabled but never launched and with no diagnostic. Bound both
	// the total wait and the total output; on either limit -- or any read
	// failure -- kill the helper, warn, and degrade to launching the job
	// without the script's environment rather than blocking indefinitely.
	const bigtime_t kReadTimeout = 15 * 1000000LL;
	const size_t kMaxOutputSize = 256 * 1024;

	// Parse into a scratch list so a partial/failed read never contributes a
	// half-populated environment: it is applied only on clean completion.
	BStringList scriptEnvironment;
	BString line;
	char buffer[4096];
	size_t totalRead = 0;
	bool failed = false;
	const bigtime_t deadline = system_time() + kReadTimeout;

	while (true) {
		bigtime_t remaining = deadline - system_time();
		if (remaining <= 0) {
			debug_printf("launch_daemon: env script \"%s\": timed out after "
				"%" B_PRIdBIGTIME " us; launching without its environment\n",
				script, kReadTimeout);
			failed = true;
			break;
		}

		struct pollfd pollData;
		pollData.fd = pipes[0];
		pollData.events = POLLIN;
		pollData.revents = 0;

		int ready = poll(&pollData, 1, remaining / 1000 + 1);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			debug_printf("launch_daemon: env script \"%s\": poll failed: %s; "
				"launching without its environment\n", script, strerror(errno));
			failed = true;
			break;
		}
		if (ready == 0) {
			// poll timed out; the outer loop re-checks the deadline
			continue;
		}

		ssize_t bytesRead = read(pipes[0], buffer, sizeof(buffer) - 1);
		if (bytesRead < 0) {
			if (errno == EINTR)
				continue;
			debug_printf("launch_daemon: env script \"%s\": read failed: %s; "
				"launching without its environment\n", script, strerror(errno));
			failed = true;
			break;
		}
		if (bytesRead == 0)
			break;

		totalRead += bytesRead;
		if (totalRead > kMaxOutputSize) {
			debug_printf("launch_daemon: env script \"%s\": output exceeded "
				"%zu bytes; launching without its environment\n", script,
				kMaxOutputSize);
			failed = true;
			break;
		}

		// Make sure the buffer is null terminated
		buffer[bytesRead] = 0;

		const char* chunk = buffer;
		while (true) {
			const char* separator = strchrnul(chunk, '\n');
			line.Append(chunk, separator - chunk);

			_ParseExportVariable(scriptEnvironment, line);
			line.Truncate(0);

			if (*separator == '\0')
				break;
			chunk = separator + 1;
		}
	}

	close(pipes[0]);

	// Reap the helper. On the failure paths it may still be running (a hanging
	// script), so terminate it first; even on the success path we must reap to
	// avoid leaking a zombie. EOF on the pipe implies the shell has closed its
	// output and is exiting, so the blocking waitpid() below returns promptly.
	if (failed)
		kill(child, SIGKILL);
	while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
		;

	if (!failed)
		environment.Add(scriptEnvironment);
}


void
BaseJob::_ParseExportVariable(BStringList& environment, const BString& line)
{
	if (!line.StartsWith("export "))
		return;

	int separator = line.FindFirst("=\"");
	if (separator < 0)
		return;

	BString variable;
	line.CopyInto(variable, 7, separator - 7);

	BString value;
	line.CopyInto(value, separator + 2, line.Length() - separator - 3);

	variable << "=" << value;
	environment.Add(variable);
}
