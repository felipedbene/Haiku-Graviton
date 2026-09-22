/*
 * Copyright 2009-2017, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include <Application.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Screen.h>
#include <Window.h>

#include "RemoteView.h"

#ifdef REMOTE_DESKTOP_TLS
#	include "WsTunnel.h"
#endif

#include <new>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


void
print_usage(const char *app)
{
	printf("usage:\t%s <host> (--cookie <cookie> | --cookie-file <path>)"
		" [-p <port>]\n\t\t[-w <width>] [-h <height>]\n", app);
	printf("usage:\t%s <host> --wss (--token <token> | --token-file <path>)"
		" [--pin <sha256>]\n\t\t[--insecure] [-p <port>] [-w <width>]"
		" [-h <height>]\n", app);
	printf("usage:\t%s <user@host> -s [<sshPort>] (--cookie <cookie> |"
		" --cookie-file <path>)\n\t\t[-p <port>] [-w <width>] [-h <height>]"
		" [-c <command>]\n", app);
	printf("usage:\t%s --wire-selftest\n", app);
	printf("\t%s --help\n\n", app);

	printf("Connect to & run applications from a different computer\n\n");
	printf("Arguments available for use:\n\n");
	printf("\t-p\t\tspecify the port to communicate on (default 10900;"
		" 10902 with --wss)\n");
	printf("\t-c\t\tsend a command to the other computer (default Terminal)\n");
	printf("\t-s\t\tuse SSH, optionally specify the SSH port to use (22)\n");
	printf("\t-w\t\tmake the virtual desktop use the specified width\n");
	printf("\t-h\t\tmake the virtual desktop use the specified height\n");
	printf("\t--wss\t\tconnect through the remote_broker daemon (TLS +"
		" WebSocket\n\t\t+ token authentication); the front door for"
		" non-loopback access\n");
	printf("\t--token\t\tthe authentication token for --wss\n");
	printf("\t--token-file\tread the authentication token from this file\n");
	printf("\t--cookie\tthe server's per-boot session cookie, required when"
		" connecting\n\t\tto the session port directly (without --wss)."
		" Prefer\n\t\t--cookie-file: an argument is visible in `ps` and in"
		" shell history\n");
	printf("\t--cookie-file\tread the session cookie from this file. On the"
		" server it is\n\t\t<system settings>/remote_desktop/session_cookie."
		"<port>, readable\n\t\tonly by the user app_server runs as; copy it"
		" over a channel you\n\t\talready trust\n");
	printf("\t--pin\t\tpin the server certificate to this SHA-256"
		" fingerprint\n\t\t(the server's broker.fingerprint file)\n");
	printf("\t--insecure\twith --wss: skip certificate pinning (still"
		" TLS)\n");
	printf("\t--wire-selftest\tcheck the URP/1 wire layer (framing,"
		" compression, the\n\t\traw/passthrough segment path and the error"
		" paths) and exit;\n\t\tno network and no display needed\n");
	printf("\nIf no width and height are specified, the window is opened with"
		" the size of the the local screen.\n");
}


// WireSelfTest.cpp
extern int remote_wire_selftest();


/*!	Reads a one-line secret (an authentication token, a session cookie) out of
	\a path into \a buffer, without the trailing newline. Returns false and
	explains itself on failure.

	A file, rather than an argument, is the recommended way to pass either: an
	argument is visible to every process on the machine through `ps` and lands
	in shell history.
*/
static bool
read_secret_file(const char *path, char *buffer, size_t bufferSize)
{
	FILE *file = fopen(path, "r");
	if (file == NULL || fgets(buffer, bufferSize, file) == NULL) {
		printf("failed to read %s\n", path);
		if (file != NULL)
			fclose(file);
		return false;
	}

	fclose(file);

	size_t length = strlen(buffer);
	while (length > 0 && (buffer[length - 1] == '\n'
			|| buffer[length - 1] == '\r' || buffer[length - 1] == ' ')) {
		buffer[--length] = '\0';
	}

	if (length == 0) {
		printf("%s is empty\n", path);
		return false;
	}

	return true;
}


int
main(int argc, char *argv[])
{
	if (argc < 2 || strcmp(argv[1], "--help") == 0) {
		print_usage(argv[0]);
		return 1;
	}

	// Before anything else: no host, no BApplication, no BScreen, so this runs
	// on a headless instance over a remote shell.
	if (strcmp(argv[1], "--wire-selftest") == 0)
		return remote_wire_selftest();

	uint16 port = 10900;
	bool portGiven = false;
	uint16 sshPort = 22;
	int32 width = -1;
	int32 height = -1;
	bool useSSH = false;
	bool useWss = false;
	bool insecure = false;
	const char *token = NULL;
	const char *tokenFile = NULL;
	const char *cookie = NULL;
	const char *cookieFile = NULL;
	const char *pin = NULL;
	const char *command = NULL;
	const char *host = argv[1];

	for (int32 i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-p") == 0) {
			if (argc <= i + 1 || sscanf(argv[i + 1], "%" B_SCNu16, &port) != 1) {
				print_usage(argv[0]);
				return 2;
			}

			portGiven = true;
			i++;
			continue;
		}

		if (strcmp(argv[i], "--wss") == 0) {
			useWss = true;
			continue;
		}

		if (strcmp(argv[i], "--token") == 0) {
			if (argc <= i + 1) {
				print_usage(argv[0]);
				return 2;
			}

			i++;
			token = argv[i];
			continue;
		}

		if (strcmp(argv[i], "--token-file") == 0) {
			if (argc <= i + 1) {
				print_usage(argv[0]);
				return 2;
			}

			i++;
			tokenFile = argv[i];
			continue;
		}

		if (strcmp(argv[i], "--cookie") == 0) {
			if (argc <= i + 1) {
				print_usage(argv[0]);
				return 2;
			}

			i++;
			cookie = argv[i];
			continue;
		}

		if (strcmp(argv[i], "--cookie-file") == 0) {
			if (argc <= i + 1) {
				print_usage(argv[0]);
				return 2;
			}

			i++;
			cookieFile = argv[i];
			continue;
		}

		if (strcmp(argv[i], "--pin") == 0) {
			if (argc <= i + 1) {
				print_usage(argv[0]);
				return 2;
			}

			i++;
			pin = argv[i];
			continue;
		}

		if (strcmp(argv[i], "--insecure") == 0) {
			insecure = true;
			continue;
		}

		if (strcmp(argv[i], "-w") == 0) {
			if (argc <= i + 1 || sscanf(argv[i + 1], "%" B_SCNd32, &width) != 1) {
				print_usage(argv[0]);
				return 2;
			}

			i++;
			continue;
		}

		if (strcmp(argv[i], "-h") == 0) {
			if (argc <= i + 1 || sscanf(argv[i + 1], "%" B_SCNd32, &height) != 1) {
				print_usage(argv[0]);
				return 2;
			}

			i++;
			continue;
		}

		if (strcmp(argv[i], "-s") == 0) {
			if((i + 1 < argc) && sscanf(argv[i + 1], "%" B_SCNu16, &sshPort) == 1) {
				i++;
			}

			useSSH = true;
			continue;
		}

		if (strcmp(argv[i], "-c") == 0) {
			if (argc <= i + 1) {
				print_usage(argv[0]);
				return 2;
			}

			i++;
			command = argv[i];
			continue;
		}

		print_usage(argv[0]);
		return 2;
	}

	if (command != NULL && !useSSH) {
		print_usage(argv[0]);
		return 2;
	}

	if (useWss && useSSH) {
		print_usage(argv[0]);
		return 2;
	}

	// The two secrets belong to two different hops and are never both ours to
	// send. Through the broker we authenticate with the token and the broker
	// presents the session cookie it reads on the server; direct to the session
	// port there is no broker, so the cookie is ours to present and the token
	// means nothing. Sending a cookie through the broker would put a second
	// cookie frame into the session stream, where the parser has no use for it.
	if (useWss && (cookie != NULL || cookieFile != NULL)) {
		printf("--cookie/--cookie-file is for a direct connection to the"
			" session port; with --wss the broker presents the cookie\n");
		return 2;
	}

	char cookieBuffer[257];
	if (!useWss) {
		if (cookieFile != NULL) {
			if (!read_secret_file(cookieFile, cookieBuffer,
					sizeof(cookieBuffer))) {
				return 6;
			}

			cookie = cookieBuffer;
		}

		if (cookie == NULL || cookie[0] == '\0') {
			printf("a direct connection to the session port requires the"
				" server's per-boot session cookie: pass --cookie-file with a"
				" copy of\n<system settings>/remote_desktop/session_cookie."
				"%" B_PRIu16 " from the server, or use --wss to go through the"
				" broker\n", port);
			return 2;
		}
	}

	// A write to a connection the peer has already dropped must be an error,
	// not a death sentence. Without this the TLS close_notify that the
	// broker transport sends while tearing a refused connection down kills
	// the process outright -- losing even the diagnostic explaining why the
	// connection was refused -- and a broker that goes away mid-session
	// would kill the client instead of ending the session.
	signal(SIGPIPE, SIG_IGN);

#ifdef REMOTE_DESKTOP_TLS
	WsTunnel tunnel;
	if (useWss) {
		if (!portGiven)
			port = 10902;

		char tokenBuffer[1024];
		if (tokenFile != NULL) {
			if (!read_secret_file(tokenFile, tokenBuffer,
					sizeof(tokenBuffer))) {
				return 6;
			}

			token = tokenBuffer;
		}

		if (token == NULL || token[0] == '\0') {
			printf("--wss requires --token or --token-file\n");
			return 2;
		}

		status_t result = tunnel.Connect(host, port, token, pin, insecure);
		if (result != B_OK)
			return 6;

		// The remaining traffic runs through the local tunnel end.
		host = "127.0.0.1";
		port = tunnel.LocalPort();
	}
#else
	(void)portGiven;
	(void)insecure;
	(void)token;
	(void)tokenFile;
	(void)pin;
	if (useWss) {
		printf("this build has no TLS support (openssl build feature was "
			"disabled)\n");
		return 6;
	}
#endif

	pid_t sshPID = -1;
	if (useSSH) {
		BPath terminalPath;
		if (command == NULL) {
			if (find_directory(B_SYSTEM_APPS_DIRECTORY, &terminalPath)
					!= B_OK) {
				printf("failed to determine system-apps directory\n");
				return 3;
			}
			if (terminalPath.Append("Terminal") != B_OK) {
				printf("failed to append to system-apps path\n");
				return 3;
			}
			command = terminalPath.Path();
		}

		char shellCommand[4096];
		snprintf(shellCommand, sizeof(shellCommand),
			"echo connected; export TARGET_SCREEN=%" B_PRIu16 "; %s\n", port,
			command);

		int pipes[4];
		if (pipe(&pipes[0]) != 0 || pipe(&pipes[2]) != 0) {
			printf("failed to create redirection pipes\n");
			return 3;
		}

		sshPID = fork();
		if (sshPID < 0) {
			printf("failed to fork ssh process\n");
			return 3;
		}

		if (sshPID == 0) {
			// child code, redirect std* and execute ssh
			close(STDOUT_FILENO);
			close(STDIN_FILENO);
			dup2(pipes[1], STDOUT_FILENO);
			dup2(pipes[1], STDERR_FILENO);
			dup2(pipes[2], STDIN_FILENO);
			for (int32 i = 0; i < 4; i++)
				close(pipes[i]);

			char localRedirect[50];
			sprintf(localRedirect, "localhost:%" B_PRIu16 ":localhost:%"
				B_PRIu16, port, port);

			char portNumber[10];
			sprintf(portNumber, "%" B_PRIu16, sshPort);

			// execl() takes a path, not a command name -- it does no PATH
			// search (see exec.cpp), so "ssh" could only ever have worked from
			// a directory containing an ssh binary. This is why -s always
			// failed with "failed to execute ssh process in child".
			int result = execl("/bin/ssh", "ssh", "-C", "-L", localRedirect,
				"-p", portNumber, "-o", "ExitOnForwardFailure=yes", host,
				shellCommand, NULL);

			// we don't get here unless there was an error in executing
			printf("failed to execute ssh process in child\n");
			return result;
		} else {
			close(pipes[1]);
			close(pipes[2]);

			char buffer[10];
			read(pipes[0], buffer, sizeof(buffer));
				// block until connected/error message from ssh

			host = "localhost";
		}
	}

	BApplication app("application/x-vnd.Haiku-RemoteDesktop");
	BRect windowFrame = BRect(0, 0, width - 1, height - 1);
	if (!windowFrame.IsValid()) {
		BScreen screen;
		windowFrame = screen.Frame();
	}

	BWindow *window = new(std::nothrow) BWindow(windowFrame, "RemoteDesktop",
		B_TITLED_WINDOW, B_QUIT_ON_WINDOW_CLOSE);

	if (window == NULL) {
		printf("no memory to allocate window\n");
		return 4;
	}

	// With --wss the connection now runs to the local end of the broker tunnel,
	// which presents the cookie itself; the view must not send a second one.
	RemoteView *view = new(std::nothrow) RemoteView(window->Bounds(), host,
		port, useWss ? NULL : cookie);
	if (view == NULL) {
		printf("no memory to allocate remote view\n");
		return 4;
	}

	status_t init = view->InitCheck();
	if (init != B_OK) {
		printf("initialization of remote view failed: %s\n", strerror(init));
		delete view;
		return 5;
	}

	window->AddChild(view);
	view->MakeFocus();
	window->Show();
	app.Run();

	if (sshPID >= 0)
		kill(sshPID, SIGHUP);

	return 0;
}
