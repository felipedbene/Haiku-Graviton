/*
 * Copyright 2009, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */
#ifndef REMOTE_VIEW_H
#define REMOTE_VIEW_H

#include <Cursor.h>
#include <NetEndpoint.h>
#include <ObjectList.h>
#include <View.h>

class BBitmap;
class BGradient;
class NetReceiver;
class NetSender;
class RemoteMessage;
class RemoteWireReader;
class StreamingRingBuffer;

struct engine_state;

// The gradient half of the RP decoder, lifted out of the drawing loop so the
// wire self-test can drive it headlessly, and so "does this opcode carry a
// gradient" has one answer instead of nine hand-written ones. See
// RemoteView.cpp.
bool			remote_opcode_has_gradient(uint16 code);
status_t		remote_read_gradient(RemoteMessage& message, uint16 code,
					BGradient** _gradient);

class RemoteView : public BView {
public:
									RemoteView(BRect frame,
										const char *remoteHost,
										uint16 remotePort,
										const char *sessionCookie = NULL);
virtual								~RemoteView();

		status_t					InitCheck();

virtual	void						AttachedToWindow();

virtual	void						Draw(BRect updateRect);

virtual	void						MouseMoved(BPoint where, uint32 code,
										const BMessage *dragMessage);
virtual	void						MouseDown(BPoint where);
virtual	void						MouseUp(BPoint where);

virtual	void						KeyDown(const char *bytes, int32 numBytes);
virtual	void						KeyUp(const char *bytes, int32 numBytes);

virtual	void						MessageReceived(BMessage *message);

private:
		void						_SendMouseMessage(uint16 code,
										BPoint where);
		void						_SendKeyMessage(uint16 code,
										const char *bytes, int32 numBytes);

static	int							_StateCompareByKey(const uint32 *key,
										const engine_state *state);
		engine_state *				_CreateState(uint32 token);
		void						_DeleteState(uint32 token);
		engine_state *				_FindState(uint32 token);

static	int32						_DrawEntry(void *data);
		void						_DrawThread();

		BRect						_BuildInvalidateRect(BPoint *points,
										int32 pointCount);

		status_t					fInitStatus;
		bool						fIsConnected;

		// The session cookie presented as the first frame on a direct (raw
		// TCP or SSH-tunnelled) connection. Empty when the connection goes
		// through the broker, which presents its own copy on our behalf.
		char						fSessionCookie[257];
		size_t						fSessionCookieLength;

		StreamingRingBuffer *		fReceiveBuffer;
		StreamingRingBuffer *		fSendBuffer;
		BNetEndpoint *				fEndpoint;
		RemoteWireReader *			fWireReader;
		NetReceiver *				fReceiver;
		NetSender *					fSender;

		bool						fStopThread;
		thread_id					fDrawThread;

		BBitmap *					fOffscreenBitmap;
		BView *						fOffscreen;

		BCursor						fViewCursor;
		BBitmap *					fCursorBitmap;
		BRect						fCursorFrame;
		bool						fCursorVisible;

		BObjectList<engine_state>	fStates;
};

#endif // REMOTE_VIEW_H
