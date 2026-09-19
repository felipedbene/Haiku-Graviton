/*
 * Copyright 2009-2010, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include "RemoteDrawingEngine.h"
#include "RemoteMessage.h"

#include "BitmapDrawingEngine.h"
#include "DrawState.h"
#include "ServerTokenSpace.h"

#include <Bitmap.h>
#include <utf8_functions.h>

#include <math.h>
#include <new>


#define TRACE(x...)				/*debug_printf("RemoteDrawingEngine: " x)*/
#define TRACE_ALWAYS(x...)		debug_printf("RemoteDrawingEngine: " x)
#define TRACE_ERROR(x...)		debug_printf("RemoteDrawingEngine: " x)


RemoteDrawingEngine::RemoteDrawingEngine(RemoteHWInterface* interface)
	:
	DrawingEngine(interface),
	fHWInterface(interface),
	fToken(gTokenSpace.NewToken(kRemoteDrawingEngineToken, this)),
	fExtendWidth(0),
	fCallbackAdded(false),
	fResultNotify(-1),
	fStringWidthResult(0.0f),
	fReadBitmapResult(NULL),
	fBitmapDrawingEngine(NULL)
{
	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_CREATE_STATE);
	message.Add(fToken);

	fHWInterface->RegisterDrawingEngine(this);
}


RemoteDrawingEngine::~RemoteDrawingEngine()
{
	// Unregister first: under the interface's engine-list lock this cannot
	// race a ConnectionReset() on a half-destroyed engine.
	fHWInterface->UnregisterDrawingEngine(this);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_DELETE_STATE);
	message.Add(fToken);
	message.Flush();

	if (fCallbackAdded)
		fHWInterface->RemoveCallback(fToken);
	if (fResultNotify >= 0)
		delete_sem(fResultNotify);
}


void
RemoteDrawingEngine::ConnectionReset()
{
	// Hold exclusive access so this cannot interleave with a drawing thread
	// mid-sequence; the freshly attached client is not drawing yet.
	bool locked = LockExclusiveAccess();

	// Recreate the per-token drawing state on the new client, which starts
	// with none.
	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_CREATE_STATE);
	message.Add(fToken);
	message.Flush();

	// Reset our cached view of that state to the client's fresh defaults, so
	// every SetXXX below re-transmits on the next repaint instead of
	// short-circuiting on a comparison against what the *previous* client had.
	DrawState defaults;
	fState.SetHighColor(defaults.HighColor());
	fState.SetLowColor(defaults.LowColor());
	fState.SetPenSize(defaults.PenSize());
	fState.SetPattern(defaults.GetPattern());
	fState.SetDrawingMode(defaults.GetDrawingMode());
	fState.SetBlendingMode(defaults.AlphaSrcMode(), defaults.AlphaFncMode());
	fState.SetLineCapMode(defaults.LineCapMode());
	fState.SetLineJoinMode(defaults.LineJoinMode());
	fState.SetMiterLimit(defaults.MiterLimit());
	fState.SetFont(defaults.Font());
	fState.SetTransform(defaults.Transform());
	fExtendWidth = -(fState.PenSize() / 2);

	// Force the next ConstrainClippingRegion() to re-send as well.
	fClippingRegion.MakeEmpty();

	if (locked)
		UnlockExclusiveAccess();
}


// #pragma mark -


void
RemoteDrawingEngine::FrameBufferChanged()
{
	// Not allowed
}


// #pragma mark -


void
RemoteDrawingEngine::SetCopyToFrontEnabled(bool enabled)
{
	DrawingEngine::SetCopyToFrontEnabled(enabled);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(enabled ? RP_ENABLE_SYNC_DRAWING : RP_DISABLE_SYNC_DRAWING);
	message.Add(fToken);
}


// #pragma mark -


//! the RemoteDrawingEngine needs to be locked!
void
RemoteDrawingEngine::ConstrainClippingRegion(const BRegion* region)
{
	if (fClippingRegion == *region)
		return;

	fClippingRegion = *region;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_CONSTRAIN_CLIPPING_REGION);
	message.Add(fToken);
	message.AddRegion(*region);
}


void
RemoteDrawingEngine::SetDrawState(const DrawState* state, int32 xOffset,
	int32 yOffset)
{
	SetPenSize(state->PenSize());
	SetDrawingMode(state->GetDrawingMode());
	SetBlendingMode(state->AlphaSrcMode(), state->AlphaFncMode());
	SetPattern(state->GetPattern().GetPattern());
	SetStrokeMode(state->LineCapMode(), state->LineJoinMode(),
		state->MiterLimit());
	SetHighColor(state->HighColor());
	SetLowColor(state->LowColor());
	SetFont(state->Font());
	SetTransform(state->CombinedTransform(), xOffset, yOffset);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_OFFSETS);
	message.Add(fToken);
	message.Add(xOffset);
	message.Add(yOffset);
}


void
RemoteDrawingEngine::SetHighColor(const rgb_color& color)
{
	if (fState.HighColor() == color)
		return;

	fState.SetHighColor(color);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_HIGH_COLOR);
	message.Add(fToken);
	message.Add(color);
}


void
RemoteDrawingEngine::SetLowColor(const rgb_color& color)
{
	if (fState.LowColor() == color)
		return;

	fState.SetLowColor(color);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_LOW_COLOR);
	message.Add(fToken);
	message.Add(color);
}


void
RemoteDrawingEngine::SetPenSize(float size)
{
	if (fState.PenSize() == size)
		return;

	fState.SetPenSize(size);
	fExtendWidth = -(size / 2);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_PEN_SIZE);
	message.Add(fToken);
	message.Add(size);
}


void
RemoteDrawingEngine::SetStrokeMode(cap_mode lineCap, join_mode joinMode,
	float miterLimit)
{
	if (fState.LineCapMode() == lineCap && fState.LineJoinMode() == joinMode
		&& fState.MiterLimit() == miterLimit)
		return;

	fState.SetLineCapMode(lineCap);
	fState.SetLineJoinMode(joinMode);
	fState.SetMiterLimit(miterLimit);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_STROKE_MODE);
	message.Add(fToken);
	message.Add(lineCap);
	message.Add(joinMode);
	message.Add(miterLimit);
}


void
RemoteDrawingEngine::SetBlendingMode(source_alpha sourceAlpha,
	alpha_function alphaFunc)
{
	if (fState.AlphaSrcMode() == sourceAlpha
		&& fState.AlphaFncMode() == alphaFunc)
		return;

	fState.SetBlendingMode(sourceAlpha, alphaFunc);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_BLENDING_MODE);
	message.Add(fToken);
	message.Add(sourceAlpha);
	message.Add(alphaFunc);
}


void
RemoteDrawingEngine::SetPattern(const struct pattern& pattern)
{
	if (fState.GetPattern() == pattern)
		return;

	fState.SetPattern(pattern);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_PATTERN);
	message.Add(fToken);
	message.Add(pattern);
}


void
RemoteDrawingEngine::SetDrawingMode(drawing_mode mode)
{
	if (fState.GetDrawingMode() == mode)
		return;

	fState.SetDrawingMode(mode);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_DRAWING_MODE);
	message.Add(fToken);
	message.Add(mode);
}


void
RemoteDrawingEngine::SetDrawingMode(drawing_mode mode, drawing_mode& oldMode)
{
	oldMode = fState.GetDrawingMode();
	SetDrawingMode(mode);
}


void
RemoteDrawingEngine::SetFont(const ServerFont& font)
{
	if (fState.Font() == font)
		return;

	fState.SetFont(font);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_FONT);
	message.Add(fToken);
	message.AddFont(font);
}


void
RemoteDrawingEngine::SetFont(const DrawState* state)
{
	SetFont(state->Font());
}


void
RemoteDrawingEngine::SetTransform(const BAffineTransform& transform,
	int32 xOffset, int32 yOffset)
{
	// TODO: take offset into account

	if (fState.Transform() == transform)
		return;

	fState.SetTransform(transform);

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_SET_TRANSFORM);
	message.Add(fToken);
	message.AddTransform(transform);
}


// #pragma mark -


BRect
RemoteDrawingEngine::CopyRect(BRect rect, int32 xOffset, int32 yOffset) const
{
	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_COPY_RECT_NO_CLIPPING);
	message.Add(xOffset);
	message.Add(yOffset);
	message.Add(rect);
	return rect.OffsetBySelf(xOffset, yOffset);
}


void
RemoteDrawingEngine::InvertRect(BRect rect)
{
	if (!fClippingRegion.Intersects(rect))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_INVERT_RECT);
	message.Add(fToken);
	message.Add(rect);
}


void
RemoteDrawingEngine::DrawBitmap(ServerBitmap* bitmap, const BRect& _bitmapRect,
	const BRect& _viewRect, uint32 options)
{
	BRect bitmapRect = _bitmapRect;
	BRect viewRect = _viewRect;
	double xScale = (bitmapRect.Width() + 1) / (viewRect.Width() + 1);
	double yScale = (bitmapRect.Height() + 1) / (viewRect.Height() + 1);

	// constrain rect to passed bitmap bounds
	// and transfer the changes to the viewRect with the right scale
	BRect actualBitmapRect = bitmap->Bounds();
	if (bitmapRect.left < actualBitmapRect.left) {
		float diff = actualBitmapRect.left - bitmapRect.left;
		viewRect.left += diff / xScale;
		bitmapRect.left = actualBitmapRect.left;
	}
	if (bitmapRect.top < actualBitmapRect.top) {
		float diff = actualBitmapRect.top - bitmapRect.top;
		viewRect.top += diff / yScale;
		bitmapRect.top = actualBitmapRect.top;
	}
	if (bitmapRect.right > actualBitmapRect.right) {
		float diff = bitmapRect.right - actualBitmapRect.right;
		viewRect.right -= diff / xScale;
		bitmapRect.right = actualBitmapRect.right;
	}
	if (bitmapRect.bottom > actualBitmapRect.bottom) {
		float diff = bitmapRect.bottom - actualBitmapRect.bottom;
		viewRect.bottom -= diff / yScale;
		bitmapRect.bottom = actualBitmapRect.bottom;
	}

	BRegion clippedRegion(viewRect);
	clippedRegion.IntersectWith(&fClippingRegion);

	int32 rectCount = clippedRegion.CountRects();
	if (rectCount == 0)
		return;

	if (rectCount > 1 || (rectCount == 1 && clippedRegion.RectAt(0) != viewRect)
		|| viewRect.Width() < bitmapRect.Width()
		|| viewRect.Height() < bitmapRect.Height()) {
		UtilityBitmap** bitmaps;
		if (_ExtractBitmapRegions(*bitmap, options, bitmapRect, viewRect,
				xScale, yScale, clippedRegion, bitmaps) != B_OK) {
			return;
		}

		RemoteMessage message(NULL, fHWInterface->SendBuffer());
		message.Start(RP_DRAW_BITMAP_RECTS);
		message.Add(fToken);
		message.Add(options);
		message.Add(bitmap->ColorSpace());
		message.Add(bitmap->Flags());
		message.Add(rectCount);

		for (int32 i = 0; i < rectCount; i++) {
			message.Add(clippedRegion.RectAt(i));
			message.AddBitmap(*bitmaps[i], true);
			delete bitmaps[i];
		}

		free(bitmaps);
		return;
	}

	// TODO: we may want to cache/checksum bitmaps

	// The fast path historically shipped the *entire* source bitmap even when
	// only bitmapRect was drawn -- a browser blit or a sprite pulled from a
	// sheet sent every pixel outside the dirty strip for nothing. Crop to the
	// integer-pixel rectangle that covers bitmapRect and rebase bitmapRect onto
	// it: the client samples pixel-identically (any sub-pixel offset is
	// preserved) while only the drawn region crosses the wire. This is a pure
	// byte saving; the wire message shape is unchanged.
	int32 cropLeft = (int32)floorf(bitmapRect.left);
	int32 cropTop = (int32)floorf(bitmapRect.top);
	int32 cropRight = (int32)ceilf(bitmapRect.right);
	int32 cropBottom = (int32)ceilf(bitmapRect.bottom);

	// bitmapRect was already constrained to actualBitmapRect above, but floor/
	// ceil of a fractional edge can land just outside it.
	cropLeft = max_c(cropLeft, (int32)actualBitmapRect.left);
	cropTop = max_c(cropTop, (int32)actualBitmapRect.top);
	cropRight = min_c(cropRight, (int32)actualBitmapRect.right);
	cropBottom = min_c(cropBottom, (int32)actualBitmapRect.bottom);

	bool wholeBitmap = cropLeft <= (int32)actualBitmapRect.left
		&& cropTop <= (int32)actualBitmapRect.top
		&& cropRight >= (int32)actualBitmapRect.right
		&& cropBottom >= (int32)actualBitmapRect.bottom;

	int32 cropWidth = cropRight - cropLeft + 1;
	int32 cropHeight = cropBottom - cropTop + 1;

	UtilityBitmap* cropped = NULL;
	if (!wholeBitmap && cropWidth > 0 && cropHeight > 0) {
		cropped = new(std::nothrow) UtilityBitmap(
			BRect(0, 0, cropWidth - 1, cropHeight - 1), bitmap->ColorSpace(), 0);
		if (cropped != NULL && cropped->ImportBits(bitmap->Bits(),
				bitmap->BitsLength(), bitmap->BytesPerRow(), bitmap->ColorSpace(),
				BPoint(cropLeft, cropTop), BPoint(0, 0), cropWidth, cropHeight)
					!= B_OK) {
			delete cropped;
			cropped = NULL;
		}
	}

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_DRAW_BITMAP);
	message.Add(fToken);
	if (cropped != NULL) {
		// Subtracting the crop origin keeps the source rect aligned to the
		// cropped pixels, so scaling to viewRect is unchanged.
		message.Add(bitmapRect.OffsetByCopy(-cropLeft, -cropTop));
		message.Add(viewRect);
		message.Add(options);
		message.AddBitmap(*cropped);
			// AddBitmap copies the pixels into the message immediately.
		delete cropped;
	} else {
		// The whole bitmap is drawn, or the crop could not be built: ship it
		// entire, exactly as before.
		message.Add(bitmapRect);
		message.Add(viewRect);
		message.Add(options);
		message.AddBitmap(*bitmap);
	}
}


void
RemoteDrawingEngine::DrawArc(BRect rect, const float& angle, const float& span,
	bool filled)
{
	BRect bounds = rect;
	if (!filled)
		bounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_ARC : RP_STROKE_ARC);
	message.Add(fToken);
	message.Add(rect);
	message.Add(angle);
	message.Add(span);
}

void
RemoteDrawingEngine::DrawArc(BRect rect, const float& angle, const float& span,
	bool filled, const BGradient& gradient)
{
	if (!fClippingRegion.Intersects(rect))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_ARC_GRADIENT : RP_STROKE_ARC_GRADIENT);
	message.Add(fToken);
	message.Add(rect);
	message.Add(angle);
	message.Add(span);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::DrawBezier(BPoint* points, bool filled)
{
	BRect bounds = _BuildBounds(points, 4);
	if (!filled)
		bounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_BEZIER : RP_STROKE_BEZIER);
	message.Add(fToken);
	message.AddList(points, 4);
}


void
RemoteDrawingEngine::DrawBezier(BPoint* points, bool filled, const BGradient& gradient)
{
	BRect bounds = _BuildBounds(points, 4);
	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_BEZIER_GRADIENT : RP_STROKE_BEZIER_GRADIENT);
	message.Add(fToken);
	message.AddList(points, 4);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::DrawEllipse(BRect rect, bool filled)
{
	BRect bounds = rect;
	if (!filled)
		bounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_ELLIPSE : RP_STROKE_ELLIPSE);
	message.Add(fToken);
	message.Add(rect);
}


void
RemoteDrawingEngine::DrawEllipse(BRect rect, bool filled, const BGradient& gradient)
{
	if (!fClippingRegion.Intersects(rect))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_ELLIPSE_GRADIENT : RP_STROKE_ELLIPSE_GRADIENT);
	message.Add(fToken);
	message.Add(rect);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::DrawPolygon(BPoint* pointList, int32 numPoints,
	BRect bounds, bool filled, bool closed)
{
	BRect clipBounds = bounds;
	if (!filled)
		clipBounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(clipBounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_POLYGON : RP_STROKE_POLYGON);
	message.Add(fToken);
	message.Add(bounds);
	message.Add(closed);
	message.Add(numPoints);
	for (int32 i = 0; i < numPoints; i++)
		message.Add(pointList[i]);
}


void
RemoteDrawingEngine::DrawPolygon(BPoint* pointList, int32 numPoints,
	BRect bounds, bool filled, bool closed, const BGradient& gradient)
{
	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_POLYGON_GRADIENT : RP_STROKE_POLYGON_GRADIENT);
	message.Add(fToken);
	message.Add(bounds);
	message.Add(closed);
	message.Add(numPoints);
	for (int32 i = 0; i < numPoints; i++)
		message.Add(pointList[i]);
	message.AddGradient(gradient);
}


// #pragma mark - rgb_color versions


void
RemoteDrawingEngine::StrokePoint(const BPoint& point, const rgb_color& color)
{
	BRect bounds(point, point);
	bounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_STROKE_POINT_COLOR);
	message.Add(fToken);
	message.Add(point);
	message.Add(color);
}


void
RemoteDrawingEngine::StrokeLine(const BPoint& start, const BPoint& end,
	const rgb_color& color)
{
	BPoint points[2] = { start, end };
	BRect bounds = _BuildBounds(points, 2);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_STROKE_LINE_1PX_COLOR);
	message.Add(fToken);
	message.AddList(points, 2);
	message.Add(color);
}


void
RemoteDrawingEngine::StrokeRect(BRect rect, const rgb_color &color)
{
	BRect bounds = rect;
	bounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_STROKE_RECT_1PX_COLOR);
	message.Add(fToken);
	message.Add(rect);
	message.Add(color);
}


void
RemoteDrawingEngine::FillRect(BRect rect, const rgb_color& color)
{
	if (!fClippingRegion.Intersects(rect))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_FILL_RECT_COLOR);
	message.Add(fToken);
	message.Add(rect);
	message.Add(color);
}


void
RemoteDrawingEngine::FillRegion(BRegion& region, const rgb_color& color)
{
	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_FILL_REGION_COLOR_NO_CLIPPING);
	message.AddRegion(region);
	message.Add(color);
}


// #pragma mark - DrawState versions


void
RemoteDrawingEngine::StrokeRect(BRect rect)
{
	BRect bounds = rect;
	bounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_STROKE_RECT);
	message.Add(fToken);
	message.Add(rect);
}


void
RemoteDrawingEngine::StrokeRect(BRect rect, const BGradient& gradient)
{
	BRect bounds = rect;
	bounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_STROKE_RECT_GRADIENT);
	message.Add(fToken);
	message.Add(rect);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::FillRect(BRect rect)
{
	if (!fClippingRegion.Intersects(rect))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_FILL_RECT);
	message.Add(fToken);
	message.Add(rect);
}


void
RemoteDrawingEngine::FillRect(BRect rect, const BGradient& gradient)
{
	if (!fClippingRegion.Intersects(rect))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_FILL_RECT_GRADIENT);
	message.Add(fToken);
	message.Add(rect);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::FillRegion(BRegion& region)
{
	BRegion clippedRegion = region;
	clippedRegion.IntersectWith(&fClippingRegion);
	if (clippedRegion.CountRects() == 0)
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_FILL_REGION);
	message.Add(fToken);
	message.AddRegion(clippedRegion.CountRects() < region.CountRects()
		? clippedRegion : region);
}


void
RemoteDrawingEngine::FillRegion(BRegion& region, const BGradient& gradient)
{
	BRegion clippedRegion = region;
	clippedRegion.IntersectWith(&fClippingRegion);
	if (clippedRegion.CountRects() == 0)
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_FILL_REGION_GRADIENT);
	message.Add(fToken);
	message.AddRegion(clippedRegion.CountRects() < region.CountRects()
		? clippedRegion : region);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::DrawRoundRect(BRect rect, float xRadius, float yRadius,
	bool filled)
{
	BRect bounds = rect;
	if (!filled)
		bounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_ROUND_RECT : RP_STROKE_ROUND_RECT);
	message.Add(fToken);
	message.Add(rect);
	message.Add(xRadius);
	message.Add(yRadius);
}


void
RemoteDrawingEngine::DrawRoundRect(BRect rect, float xRadius, float yRadius,
	bool filled, const BGradient& gradient)
{
	if (!fClippingRegion.Intersects(rect))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_ROUND_RECT_GRADIENT : RP_STROKE_ROUND_RECT_GRADIENT);
	message.Add(fToken);
	message.Add(rect);
	message.Add(xRadius);
	message.Add(yRadius);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::DrawShape(const BRect& bounds, int32 opCount,
	const uint32* opList, int32 pointCount, const BPoint* pointList,
	bool filled, const BPoint& viewToScreenOffset, float viewScale)
{
	BRect clipBounds = bounds;
	if (!filled)
		clipBounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(clipBounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_SHAPE : RP_STROKE_SHAPE);
	message.Add(fToken);
	message.Add(bounds);
	message.Add(opCount);
	message.AddList(opList, opCount);
	message.Add(pointCount);
	message.AddList(pointList, pointCount);
	message.Add(viewToScreenOffset);
	message.Add(viewScale);
}


void
RemoteDrawingEngine::DrawShape(const BRect& bounds, int32 opCount,
	const uint32* opList, int32 pointCount, const BPoint* pointList,
	bool filled, const BGradient& gradient, const BPoint& viewToScreenOffset,
	float viewScale)
{
	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_SHAPE_GRADIENT : RP_STROKE_SHAPE_GRADIENT);
	message.Add(fToken);
	message.Add(bounds);
	message.Add(opCount);
	message.AddList(opList, opCount);
	message.Add(pointCount);
	message.AddList(pointList, pointCount);
	message.Add(viewToScreenOffset);
	message.Add(viewScale);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::DrawTriangle(BPoint* points, const BRect& bounds,
	bool filled)
{
	BRect clipBounds = bounds;
	if (!filled)
		clipBounds.InsetBy(fExtendWidth, fExtendWidth);

	if (!fClippingRegion.Intersects(clipBounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_TRIANGLE : RP_STROKE_TRIANGLE);
	message.Add(fToken);
	message.AddList(points, 3);
	message.Add(bounds);
}


void
RemoteDrawingEngine::DrawTriangle(BPoint* points, const BRect& bounds,
	bool filled, const BGradient& gradient)
{
	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(filled ? RP_FILL_TRIANGLE_GRADIENT : RP_STROKE_TRIANGLE_GRADIENT);
	message.Add(fToken);
	message.AddList(points, 3);
	message.Add(bounds);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::StrokeLine(const BPoint &start, const BPoint &end)
{
	BPoint points[2] = { start, end };
	BRect bounds = _BuildBounds(points, 2);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_STROKE_LINE);
	message.Add(fToken);
	message.AddList(points, 2);
}


void
RemoteDrawingEngine::StrokeLine(const BPoint &start, const BPoint &end, const BGradient& gradient)
{
	BPoint points[2] = { start, end };
	BRect bounds = _BuildBounds(points, 2);

	if (!fClippingRegion.Intersects(bounds))
		return;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_STROKE_LINE_GRADIENT);
	message.Add(fToken);
	message.AddList(points, 2);
	message.AddGradient(gradient);
}


void
RemoteDrawingEngine::StrokeLineArray(int32 numLines,
	const ViewLineArrayInfo *lineData)
{
	RemoteMessage message(NULL, fHWInterface->SendBuffer());
	message.Start(RP_STROKE_LINE_ARRAY);
	message.Add(fToken);
	message.Add(numLines);
	for (int32 i = 0; i < numLines; i++)
		message.AddArrayLine(lineData[i]);
}


// #pragma mark - string functions


BPoint
RemoteDrawingEngine::DrawString(const char* string, int32 length,
	const BPoint& point, escapement_delta* delta)
{
	RemoteMessage message(NULL, fHWInterface->SendBuffer());

	message.Start(RP_DRAW_STRING);
	message.Add(fToken);
	message.Add(point);
	message.AddString(string, length);
	message.Add(delta != NULL);
	if (delta != NULL)
		message.Add(delta[0]);
		// escapement_delta is a single struct applied to the whole string, not
		// one per glyph. The old AddList(delta, length) read `length` structs
		// from a one-element pointer -- an out-of-bounds stack read that also
		// put 8*(length-1) junk bytes on the wire. Send exactly the one delta
		// (defect D5).

	// No client is attached, so nothing will ever answer: skip the wait rather
	// than stall the drawing thread for the full timeout on every string.
	if (!fHWInterface->IsConnected())
		return point;

	status_t result = _AddCallback();
	if (message.Flush() != B_OK)
		return point;

	if (result != B_OK)
		return point;

	_DrainResultSem();

	do {
		result = acquire_sem_etc(fResultNotify, 1, B_RELATIVE_TIMEOUT,
			1 * 1000 * 1000);
	} while (result == B_INTERRUPTED);

	if (result != B_OK)
		return point;

	return fDrawStringResult;
}


BPoint
RemoteDrawingEngine::DrawString(const char* string, int32 length,
	const BPoint* offsets)
{
	// Guaranteed to have at least one point.
	RemoteMessage message(NULL, fHWInterface->SendBuffer());

	message.Start(RP_DRAW_STRING_WITH_OFFSETS);
	message.Add(fToken);
	message.AddString(string, length);
	message.AddList(offsets, UTF8CountChars(string, length));

	// No client is attached, so nothing will ever answer: skip the wait rather
	// than stall the drawing thread for the full timeout on every string.
	if (!fHWInterface->IsConnected())
		return offsets[0];

	status_t result = _AddCallback();
	if (message.Flush() != B_OK)
		return offsets[0];

	if (result != B_OK)
		return offsets[0];

	_DrainResultSem();

	do {
		result = acquire_sem_etc(fResultNotify, 1, B_RELATIVE_TIMEOUT,
			1 * 1000 * 1000);
	} while (result == B_INTERRUPTED);

	if (result != B_OK)
		return offsets[0];

	return fDrawStringResult;
}


float
RemoteDrawingEngine::StringWidth(const char* string, int32 length,
	escapement_delta* delta)
{
	// TODO: Decide if really needed.

	while (true) {
		if (_AddCallback() != B_OK)
			break;

		RemoteMessage message(NULL, fHWInterface->SendBuffer());

		message.Start(RP_STRING_WIDTH);
		message.Add(fToken);
		message.AddString(string, length);
			// TODO: Support escapement delta.

		if (message.Flush() != B_OK)
			break;

		status_t result;
		do {
			result = acquire_sem_etc(fResultNotify, 1, B_RELATIVE_TIMEOUT,
				1 * 1000 * 1000);
		} while (result == B_INTERRUPTED);

		if (result != B_OK)
			break;

		return fStringWidthResult;
	}

	// Fall back to local calculation.
	return fState.Font().StringWidth(string, length, delta);
}


// #pragma mark -


status_t
RemoteDrawingEngine::ReadBitmap(ServerBitmap* bitmap, bool drawCursor,
	BRect bounds)
{
	// With no client attached there is no framebuffer to read back; do not stall
	// the caller (a screenshot) for the full 10 s timeout waiting for a reply
	// that cannot come (defect D1, the headless case).
	if (!fHWInterface->IsConnected())
		return B_UNSUPPORTED;

	if (_AddCallback() != B_OK)
		return B_UNSUPPORTED;

	RemoteMessage message(NULL, fHWInterface->SendBuffer());

	message.Start(RP_READ_BITMAP);
	message.Add(fToken);
	message.Add(bounds);
	message.Add(drawCursor);
	if (message.Flush() != B_OK)
		return B_UNSUPPORTED;

	_DrainResultSem();

	status_t result;
	do {
		result = acquire_sem_etc(fResultNotify, 1, B_RELATIVE_TIMEOUT,
			10 * 1000 * 1000);
	} while (result == B_INTERRUPTED);

	if (result != B_OK)
		return result;

	BBitmap* read = fReadBitmapResult;
	if (read == NULL)
		return B_UNSUPPORTED;

	result = bitmap->ImportBits(read->Bits(), read->BitsLength(),
		read->BytesPerRow(), read->ColorSpace());
	delete read;
	return result;
}


// #pragma mark -


status_t
RemoteDrawingEngine::_AddCallback()
{
	if (fCallbackAdded)
		return B_OK;

	if (fResultNotify < 0)
		fResultNotify = create_sem(0, "drawing engine result");
	if (fResultNotify < 0)
		return fResultNotify;

	status_t result = fHWInterface->AddCallback(fToken, &_DrawingEngineResult,
		this);

	fCallbackAdded = result == B_OK;
	return result;
}


void
RemoteDrawingEngine::_DrainResultSem()
{
	// A previous synchronous call may have timed out and returned before its
	// reply arrived. When that late reply finally lands, the callback releases
	// fResultNotify -- and without this that stale release would satisfy the
	// *next* call's wait, handing it the previous call's result (defect D8).
	// Call this after flushing the request and before waiting: this call's own
	// reply is a full network round trip away and cannot be here yet, so any
	// count pending now is necessarily that stale release. Discard it.
	if (fResultNotify < 0)
		return;

	while (acquire_sem_etc(fResultNotify, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;
}


bool
RemoteDrawingEngine::_DrawingEngineResult(void* cookie, RemoteMessage& message)
{
	RemoteDrawingEngine* engine = (RemoteDrawingEngine*)cookie;

	switch (message.Code()) {
		case RP_DRAW_STRING_RESULT:
		{
			status_t result = message.Read(engine->fDrawStringResult);
			if (result != B_OK) {
				TRACE_ERROR("failed to read draw string result: %s\n",
					strerror(result));
				return false;
			}

			break;
		}

		case RP_STRING_WIDTH_RESULT:
		{
			status_t result = message.Read(engine->fStringWidthResult);
			if (result != B_OK) {
				TRACE_ERROR("failed to read string width result: %s\n",
					strerror(result));
				return false;
			}

			break;
		}

		case RP_READ_BITMAP_RESULT:
		{
			status_t result = message.ReadBitmap(&engine->fReadBitmapResult);
			if (result != B_OK) {
				TRACE_ERROR("failed to read bitmap of read bitmap result: %s\n",
					strerror(result));
				return false;
			}

			break;
		}

		default:
			return false;
	}

	release_sem(engine->fResultNotify);
	return true;
}


BRect
RemoteDrawingEngine::_BuildBounds(BPoint* points, int32 pointCount)
{
	BRect bounds(1000000, 1000000, 0, 0);
	for (int32 i = 0; i < pointCount; i++) {
		bounds.left = min_c(bounds.left, points[i].x);
		bounds.top = min_c(bounds.top, points[i].y);
		bounds.right = max_c(bounds.right, points[i].x);
		bounds.bottom = max_c(bounds.bottom, points[i].y);
	}

	return bounds;
}


status_t
RemoteDrawingEngine::_ExtractBitmapRegions(ServerBitmap& bitmap, uint32 options,
	const BRect& bitmapRect, const BRect& viewRect, double xScale,
	double yScale, BRegion& region, UtilityBitmap**& bitmaps)
{
	int32 rectCount = region.CountRects();
	bitmaps = (UtilityBitmap**)malloc(rectCount * sizeof(UtilityBitmap*));
	if (bitmaps == NULL)
		return B_NO_MEMORY;

	for (int32 i = 0; i < rectCount; i++) {
		BRect sourceRect = region.RectAt(i).OffsetByCopy(-viewRect.LeftTop());
		int32 targetWidth = (int32)(sourceRect.Width() + 1.5);
		int32 targetHeight = (int32)(sourceRect.Height() + 1.5);

		if (xScale != 1.0) {
			sourceRect.left = (int32)(sourceRect.left * xScale + 0.5);
			sourceRect.right = (int32)(sourceRect.right * xScale + 0.5);
			if (xScale < 1.0)
				targetWidth = (int32)(sourceRect.Width() + 1.5);
		}

		if (yScale != 1.0) {
			sourceRect.top = (int32)(sourceRect.top * yScale + 0.5);
			sourceRect.bottom = (int32)(sourceRect.bottom * yScale + 0.5);
			if (yScale < 1.0)
				targetHeight = (int32)(sourceRect.Height() + 1.5);
		}

		sourceRect.OffsetBy(bitmapRect.LeftTop());
			// sourceRect is now the part of the bitmap we want copied

		status_t result = B_OK;
		if ((xScale > 1.0 || yScale > 1.0)
			&& (targetWidth * targetHeight < (int32)(sourceRect.Width() + 1.5)
				* (int32)(sourceRect.Height() + 1.5))) {
			// the target bitmap is smaller than the source, scale it locally
			// and send over the smaller version to avoid sending any extra data
			if (!fBitmapDrawingEngine.IsSet()) {
				fBitmapDrawingEngine.SetTo(
					new(std::nothrow) BitmapDrawingEngine(B_RGBA32));
				if (!fBitmapDrawingEngine.IsSet())
					result = B_NO_MEMORY;
			}

			if (result == B_OK) {
				result = fBitmapDrawingEngine->SetSize(targetWidth,
					targetHeight);
			}

			if (result == B_OK) {
				fBitmapDrawingEngine->SetDrawingMode(B_OP_COPY);

				switch (bitmap.ColorSpace()) {
					case B_RGBA32:
					case B_RGBA32_BIG:
					case B_RGBA15:
					case B_RGBA15_BIG:
						break;

					default:
					{
						// we need to clear the background if there may be
						// transparency through transparent magic (we use
						// B_OP_COPY when we draw alpha enabled bitmaps, so we
						// don't need to clear there)
						// TODO: this is not actually correct, as we're going to
						// loose the transparency with the conversion to the
						// original non-alpha colorspace happening in
						// ExportToBitmap
						rgb_color background = { 0, 0, 0, 0 };
						fBitmapDrawingEngine->FillRect(
							BRect(0, 0, targetWidth - 1, targetHeight -1),
							background);
						fBitmapDrawingEngine->SetDrawingMode(B_OP_OVER);
						break;
					}
				}

				fBitmapDrawingEngine->DrawBitmap(&bitmap, sourceRect,
					BRect(0, 0, targetWidth - 1, targetHeight - 1), options);
				bitmaps[i] = fBitmapDrawingEngine->ExportToBitmap(targetWidth,
					targetHeight, bitmap.ColorSpace());
				if (bitmaps[i] == NULL)
					result = B_NO_MEMORY;
			}
		} else {
			// source is smaller or equal target, extract the relevant rects
			// directly without any scaling and conversion
			targetWidth = (int32)(sourceRect.Width() + 1.5);
			targetHeight = (int32)(sourceRect.Height() + 1.5);

			bitmaps[i] = new(std::nothrow) UtilityBitmap(
				BRect(0, 0, targetWidth - 1, targetHeight - 1),
				bitmap.ColorSpace(), 0);
			if (bitmaps[i] == NULL) {
				result = B_NO_MEMORY;
			} else {
				result = bitmaps[i]->ImportBits(bitmap.Bits(),
						bitmap.BitsLength(), bitmap.BytesPerRow(),
						bitmap.ColorSpace(), sourceRect.LeftTop(),
						BPoint(0, 0), targetWidth, targetHeight);
				if (result != B_OK) {
					delete bitmaps[i];
					bitmaps[i] = NULL;
				}
			}
		}

		if (result != B_OK) {
			for (int32 j = 0; j < i; j++)
				delete bitmaps[j];
			free(bitmaps);
			return result;
		}
	}

	return B_OK;
}
