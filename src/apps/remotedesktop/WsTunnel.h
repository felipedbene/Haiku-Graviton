/*
 * Copyright 2026, DeBeOS.
 * Distributed under the terms of the MIT License.
 */
#ifndef WS_TUNNEL_H
#define WS_TUNNEL_H

#include <OS.h>
#include <SupportDefs.h>

#include "TLSStream.h"
#include "WebSocketStream.h"


/*!	The client end of the remote_broker transport: connects out with TLS
	(trust = certificate pinning), performs the WebSocket and
	RP_AUTHENTICATE handshakes, and then exposes the decapsulated remote
	protocol byte stream on a local loopback port, so the existing
	NetSender/NetReceiver based RemoteView connects to it unchanged.
*/
class WsTunnel {
public:
								WsTunnel();
								~WsTunnel();

			// Performs the complete outbound handshake. With a NULL
			// \a pinnedFingerprint and \a insecure false this fails after
			// printing the observed fingerprint, so the user can verify it
			// out-of-band and pass it back in.
			status_t			Connect(const char* host, uint16 port,
									const char* token,
									const char* pinnedFingerprint,
									bool insecure);

			// Valid after a successful Connect(): where the local client
			// should connect to.
			uint16				LocalPort() const { return fLocalPort; }

private:
	static	int32				_PumpEntry(void* data);
			status_t			_Pump();

			TLSContext			fContext;
			TLSStream			fStream;
			WebSocketStream		fWebSocket;
			int					fListenSocket;
			uint16				fLocalPort;
			thread_id			fPumpThread;
};


#endif // WS_TUNNEL_H
