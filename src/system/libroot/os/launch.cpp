/*
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include <launch.h>

#include <TokenSpace.h>
#include <util/KMessage.h>


static port_id sLaunchDaemonPort = -1;

/*!	How long a request to the launch daemon will wait before giving up.

	Same reasoning as kRegistrarRequestTimeout in
	src/system/libroot/posix/user_group_common.cpp (issue 770): an unbounded
	wait here is what turns a wedged service into an unreachable machine. This
	is the other half of the user/group lookup path -- the first getpwnam() in
	any process has no cached registrar port yet, so it comes through here, and
	sshd forks per connection. Bounding only the registrar round trip would move
	the hang rather than remove it.

	The daemon is single-threaded (LaunchDaemon is a BServer looper, so
	MessageReceived runs serially), and B_GET_LAUNCH_DATA can do real work
	before it answers: for a service that is not running yet it calls
	_LaunchJob() inline (src/servers/launch/LaunchDaemon.cpp:764) and then hands
	the reply to the job (:767), i.e. the caller is not answered until the
	service comes up and registers its port. On a full volume that can be never.

	Hence the longer value than the registrar's 30 s: the warm case is cheap --
	graviton/scripts/pw770.c measures 135 us for its very first lookup, which is
	the one that includes this round trip -- but the cold case legitimately
	covers spawning and linking a server, which is not something measured here.
	60 s is deliberately generous for that, and still finite.
*/
static const bigtime_t kLaunchDaemonRequestTimeout = 60 * 1000000LL;


port_id
BPrivate::get_launch_daemon_port()
{
	if (sLaunchDaemonPort < 0) {
		sLaunchDaemonPort = find_port(B_LAUNCH_DAEMON_PORT_NAME);

		port_info info;
		if (get_port_info(sLaunchDaemonPort, &info) == B_OK
			&& info.team == find_thread(NULL)) {
			// Make sure that the launch_daemon doesn't wait on itself
			sLaunchDaemonPort = -1;
			return -1;
		}
	}

	return sLaunchDaemonPort;
}


status_t
BPrivate::send_request_to_launch_daemon(KMessage& request, KMessage& reply)
{
	// Bound both halves: if the daemon's looper is stuck, its port queue fills
	// and the delivery is what blocks, so timing out only the reply would still
	// hang here.
	status_t status = request.SendTo(get_launch_daemon_port(),
		B_PREFERRED_TOKEN, &reply, kLaunchDaemonRequestTimeout,
		kLaunchDaemonRequestTimeout);
	if (status != B_OK)
		return status;

	return (status_t)reply.What();
}


status_t
BPrivate::get_launch_data(const char* signature, KMessage& data)
{
	BPrivate::KMessage request(B_GET_LAUNCH_DATA);
	request.AddString("name", signature);

	return BPrivate::send_request_to_launch_daemon(request, data);
}
