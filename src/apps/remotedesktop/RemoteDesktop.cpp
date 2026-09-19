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
	printf("usage:\t%s <host> [-p <port>] [-w <width>] [-h <height>]\n", app);
	printf("usage:\t%s <host> --wss (--token <token> | --token-file <path>)"
		" [--pin <sha256>]\n\t\t[--insecure] [-p <port>] [-w <width>]"
		" [-h <height>]\n", app);
	printf("usage:\t%s <user@host> -s [<sshPort>] [-p <port>] [-w <width>]"
		" [-h <height>] [-c <command>]\n", app);
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
	printf("\t--pin\t\tpin the server certificate to this SHA-256"
		" fingerprint\n\t\t(the server's broker.fingerprint file)\n");
	printf("\t--insecure\twith --wss: skip certificate pinning (still"
		" TLS)\n");
	printf("\nIf no width and height are specified, the window is opened with"
		" the size of the the local screen.\n");
}


int
main(int argc, char *argv[])
{
	if (argc < 2 || strcmp(argv[1], "--help") == 0) {
		print_usage(argv[0]);
		return 1;
	}

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
			FILE* file = fopen(tokenFile, "r");
			if (file == NULL || fgets(tokenBuffer, sizeof(tokenBuffer),
					file) == NULL) {
				printf("failed to read token file %s\n", tokenFile);
				if (file != NULL)
					fclose(file);
				return 6;
			}
			fclose(file);

			size_t length = strlen(tokenBuffer);
			while (length > 0 && (tokenBuffer[length - 1] == '\n'
					|| tokenBuffer[length - 1] == '\r'
					|| tokenBuffer[length - 1] == ' ')) {
				tokenBuffer[--length] = '\0';
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

	RemoteView *view = new(std::nothrow) RemoteView(window->Bounds(), host,
		port);
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
