/*
 * Copyright 2009, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */
#ifndef REMOTE_MESSAGE_H
#define REMOTE_MESSAGE_H

#ifndef CLIENT_COMPILE
#	include "PatternHandler.h"
#	include "RemoteWireWriter.h"
#	include <ViewPrivate.h>
#endif

#include "StreamingRingBuffer.h"

#include <AffineTransform.h>
#include <GraphicsDefs.h>
#include <Region.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

class BBitmap;
class BFont;
class BGradient;
class BView;
class DrawState;
class Pattern;
class RemotePainter;
class ServerBitmap;
class ServerCursor;
class ServerFont;
struct ViewLineArrayInfo;

#include "RemoteProtocol.h"


class RemoteMessage {
public:
								RemoteMessage(StreamingRingBuffer* source,
									StreamingRingBuffer *target);
#ifndef CLIENT_COMPILE
								/*!	Server-side outbound messages go through
									the wire writer, which owns the compression
									state and the ordering guarantee. */
								RemoteMessage(StreamingRingBuffer* source,
									RemoteWireWriter *target);
#endif
								~RemoteMessage();

		void					Start(uint16 code);
		status_t				Flush();
#ifndef CLIENT_COMPILE
		status_t				FlushAndEnableCompression(uint32 capability);
#endif
		void					Cancel();

		status_t				NextMessage(uint16& code);
		void					Reset();
		bool					ResetIfGenerationChanged(uint32 generation);
		uint16					Code() { return fCode; }
		uint32					DataLeft() { return fDataLeft; }

		template<typename T>
		void					Add(const T& value);

		void					AddString(const char* string, size_t length);
		void					AddRegion(const BRegion& region);
		void					AddGradient(const BGradient& gradient);
		void					AddTransform(const BAffineTransform& transform);

#ifndef CLIENT_COMPILE
		void					AddBitmap(const ServerBitmap& bitmap,
									bool minimal = false);
		void					AddFont(const ServerFont& font);
		void					AddPattern(const Pattern& pattern);
		void					AddDrawState(const DrawState& drawState);
		void					AddArrayLine(const ViewLineArrayInfo& line);
		void					AddCursor(const ServerCursor& cursor);
#else
		void					AddBitmap(const BBitmap& bitmap);
#endif

		template<typename T>
		void					AddList(const T* array, int32 count);

		template<typename T>
		status_t				Read(T& value);

		status_t				ReadRegion(BRegion& region);
		status_t				ReadFontState(BFont& font);
									// sets font state
		status_t				ReadViewState(BView& view, ::pattern& pattern);
									// sets viewstate and returns pattern

		status_t				ReadString(char** _string, size_t& length);
		status_t				ReadBitmap(BBitmap** _bitmap,
									bool minimal = false,
									color_space colorSpace = B_RGB32,
									uint32 flags = 0);
		status_t				ReadGradient(BGradient** _gradient);
		status_t				ReadTransform(BAffineTransform& transform);
		status_t				ReadArrayLine(BPoint& startPoint,
									BPoint& endPoint, rgb_color& color);

		template<typename T>
		status_t				ReadList(T* array, int32 count);

private:
		bool					_MakeSpace(size_t size);

		StreamingRingBuffer*	fSource;
		StreamingRingBuffer*	fTarget;
#ifndef CLIENT_COMPILE
		RemoteWireWriter*		fWireTarget;
#endif

		uint8*					fBuffer;
		size_t					fAvailable;
		size_t					fWriteIndex;
		uint32					fDataLeft;
		uint16					fCode;
		uint32					fGeneration;
};


inline
RemoteMessage::RemoteMessage(StreamingRingBuffer* source,
	StreamingRingBuffer* target)
	:
	fSource(source),
	fTarget(target),
#ifndef CLIENT_COMPILE
	fWireTarget(NULL),
#endif
	fBuffer(NULL),
	fAvailable(0),
	fWriteIndex(0),
	fDataLeft(0),
	fCode(0),
	fGeneration(0)
{
}


#ifndef CLIENT_COMPILE
inline
RemoteMessage::RemoteMessage(StreamingRingBuffer* source,
	RemoteWireWriter* target)
	:
	fSource(source),
	fTarget(NULL),
	fWireTarget(target),
	fBuffer(NULL),
	fAvailable(0),
	fWriteIndex(0),
	fDataLeft(0),
	fCode(0),
	fGeneration(0)
{
}
#endif


inline
RemoteMessage::~RemoteMessage()
{
	if (fWriteIndex > 0)
		Flush();
	free(fBuffer);
}


inline void
RemoteMessage::Start(uint16 code)
{
	if (fWriteIndex > 0)
		Flush();

	Add(code);

	uint32 sizeDummy = 0;
	Add(sizeDummy);
}


inline status_t
RemoteMessage::Flush()
{
#ifdef CLIENT_COMPILE
	if (fWriteIndex == 0 || fTarget == NULL)
		return B_NO_INIT;
#else
	if (fWriteIndex == 0 || (fTarget == NULL && fWireTarget == NULL))
		return B_NO_INIT;
#endif

	uint32 length = fWriteIndex;
	fAvailable += fWriteIndex;
	fWriteIndex = 0;

	memcpy(fBuffer + sizeof(uint16), &length, sizeof(uint32));

#ifndef CLIENT_COMPILE
	if (fWireTarget != NULL)
		return fWireTarget->Write(fBuffer, length);
#endif

	return fTarget->Write(fBuffer, length);
}


#ifndef CLIENT_COMPILE
/*!	Flushes this message and, atomically with it, switches the outbound stream
	to compressed segments. Only meaningful for RP_HELLO_ACK: see
	RemoteWireWriter::WriteAndEnable() for why the two steps cannot be separate.
*/
inline status_t
RemoteMessage::FlushAndEnableCompression(uint32 capability)
{
	if (fWriteIndex == 0 || fWireTarget == NULL)
		return B_NO_INIT;

	uint32 length = fWriteIndex;
	fAvailable += fWriteIndex;
	fWriteIndex = 0;

	memcpy(fBuffer + sizeof(uint16), &length, sizeof(uint32));
	return fWireTarget->WriteAndEnable(fBuffer, length, capability);
}
#endif


template<typename T>
inline void
RemoteMessage::Add(const T& value)
{
	if (!_MakeSpace(sizeof(T)))
		return;

	memcpy(fBuffer + fWriteIndex, &value, sizeof(T));
	fWriteIndex += sizeof(T);
	fAvailable -= sizeof(T);
}


inline void
RemoteMessage::AddString(const char* string, size_t length)
{
	Add((uint32)length);
	if (length > fAvailable && !_MakeSpace(length))
		return;

	memcpy(fBuffer + fWriteIndex, string, length);
	fWriteIndex += length;
	fAvailable -= length;
}


inline void
RemoteMessage::AddRegion(const BRegion& region)
{
	int32 rectCount = region.CountRects();
	Add(rectCount);

	for (int32 i = 0; i < rectCount; i++)
		Add(region.RectAt(i));
}


template<typename T>
inline void
RemoteMessage::AddList(const T* array, int32 count)
{
	for (int32 i = 0; i < count; i++)
		Add(array[i]);
}


template<typename T>
inline status_t
RemoteMessage::Read(T& value)
{
	if (fDataLeft < sizeof(T))
		return B_ERROR;

	if (fSource == NULL)
		return B_NO_INIT;

	int32 readSize = fSource->Read(&value, sizeof(T));
	if (readSize < 0)
		return readSize;

	if (readSize != sizeof(T))
		return B_ERROR;

	fDataLeft -= sizeof(T);
	return B_OK;
}


inline status_t
RemoteMessage::ReadRegion(BRegion& region)
{
	region.MakeEmpty();

	int32 rectCount;
	status_t result = Read(rectCount);
	if (result != B_OK)
		return B_ERROR;

	for (int32 i = 0; i < rectCount; i++) {
		BRect rect;
		status_t result = Read(rect);
		if (result != B_OK)
			return result;

		region.Include(rect);
	}

	return B_OK;
}


template<typename T>
inline status_t
RemoteMessage::ReadList(T* array, int32 count)
{
	for (int32 i = 0; i < count; i++) {
		status_t result = Read(array[i]);
		if (result != B_OK)
			return result;
	}

	return B_OK;
}


inline bool
RemoteMessage::_MakeSpace(size_t size)
{
	if (fAvailable >= size)
		return true;

	size_t extraSize = size + 20;
	uint8 *newBuffer = (uint8*)realloc(fBuffer, fWriteIndex + extraSize);
	if (newBuffer == NULL)
		return false;

	fAvailable = extraSize;
	fBuffer = newBuffer;
	return true;
}

#endif // REMOTE_MESSAGE_H
