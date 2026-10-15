/*
 * Copyright 2026, DeBeOS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	A deterministic drawing workload for the RP_SET_OFFSETS replay proof (#544).

	A titled window at a non-zero screen origin with two sibling views at
	different pen-to-screen origins, each drawing through a non-identity view
	transform. The offsets are the origin a remote client conjugates the
	transform about (p' = origin + M * (p - origin), identically in
	Painter::SetTransform() and in HaikuRemoteDesktop.js's
	translate/transform/translate bracket), so a client that loses them after a
	reconnect does not go blank -- it draws the same figure in the wrong place.
	One view could not tell a lost offset from a wrong constant; two views whose
	origins differ pin it.

	The transform is a pure SCALE rather than a rotation. app_server does not
	care which, but a scale maps an axis-aligned rectangle to an axis-aligned
	rectangle, and the conjugation then contributes exactly
	origin * (1 - scale) of translation -- a displacement a rasteriser working
	in boxes can be held to at the pixel level (rdcapture --view-transform). A
	rotation would force the comparison back onto path rendering and buy nothing
	for the defect under test.

	No text and no bitmaps: both would make the framebuffer depend on fonts or
	on resources, and the comparison here has to be exact.

	WHAT THIS WORKLOAD CAN AND CANNOT MEASURE -- read this before concluding
	anything from a passing run. Both halves were measured under QEMU, not
	reasoned about.

	It CAN show that the replay does not regress the raster: two framebuffers
	captured across a reconnect are byte-identical.

	It CANNOT fail if the replay is removed. An app_server built with the dedup
	guard and WITHOUT the RP_SET_OFFSETS line in ReplayState() -- the deliberate
	D4 reintroduction -- captured byte-identically here, over a reconnect, to one
	built with both. The reason is that the offsets are only put on the wire when
	app_server switches the current view (ServerWindow.cpp:4507-4514 is the only
	caller of SetDrawState that supplies them; Canvas.cpp:296 and Layer.cpp:45
	pass none, and TabDecorator.cpp:105,122 pass none either), and app_server
	sends a full invalidate on reconnect. A repaint that touches two origins
	therefore emits an offsets *change* at the head of it and re-teaches the
	origin to the new client by the ordinary drawing path, replay or no replay.

	Removing the second view does not help, and makes it worse: a single-view
	borderless window emits NO RP_SET_OFFSETS at all, on either build, because
	there is no view switch to trigger one. Measured on a window at (120,90)
	drawing through a scale of 2: the wire carried RP_FILL_RECT at the correct
	screen rect (128,98)-(168,128) and RP_SET_TRANSFORM with m = 2, and no
	offsets, so a conforming client conjugates about (0,0) and puts the figure at
	(256,196)-(336,256) -- about 120 px out, on the FIRST connection, with no
	reconnect involved. That is a pre-existing hole in the remote path rather
	than anything this change introduces (before the guard, SetDrawState sent
	RP_SET_OFFSETS unconditionally, but with the same (0,0) it was handed), and
	the mechanism is not yet pinned down. It is recorded here because it is the
	reason a one-view workload looks like the obvious test and is in fact no test
	at all.

	So the mutation arm for #544 is asserted ON THE WIRE, not on the raster, by
	offsets-replay-gate.py in this directory: the offsets for every transformed
	state must arrive in the reconnect burst's REPLAY PROLOGUE, ahead of the
	burst's first drawing op. Measured: the fixed build passes on both the first
	connection and the reconnect, the build with the ReplayState() line removed
	fails on the reconnect. See that script's own header for why the obvious
	weaker form of the assertion -- "offsets somewhere before that state's first
	op" -- passes on the broken build as well, and by luck rather than by
	correctness.

	HOW TO RUN IT. Stage this app and relay.cpp's binary into an image (a
	UserBuildConfig that copies them into system non-packaged bin, plus launch
	jobs, is the usual way), boot it, take the pair, and gate the wire:

		rdcapture.py --host 127.0.0.1 --port <relay> --cookie <hex> \
			--width 800 --height 600 --seconds 20 --view-transform \
			--no-glyphs --allow-text-estimate --png A.png --wire-dump A.wire
		# drop the connection, then repeat into B.png / B.wire
		offsets-replay-gate.py A.wire=first B.wire=reconnect

	The cookie is app_server's per-boot session cookie (#423), published to
	<system settings>/remote_desktop/session_cookie.<port>.
*/


#include <AffineTransform.h>
#include <Application.h>
#include <View.h>
#include <Window.h>


static const char* kAppSignature
	= "application/x-vnd.DeBeOS-remote_offsets";

// Large enough that origin * (1 - scale) is a displacement no antialiasing or
// rounding difference could be mistaken for.
static const double kScale = 2.0;


class TransformView : public BView {
public:
	TransformView(BRect frame, const char* name, rgb_color fill,
		rgb_color stroke)
		:
		BView(frame, name, B_FOLLOW_NONE, B_WILL_DRAW),
		fFill(fill),
		fStroke(stroke)
	{
		SetViewColor(240, 240, 240);
	}

	virtual void Draw(BRect updateRect)
	{
		BAffineTransform transform;
		transform.ScaleBy(kScale, kScale);
		SetTransform(transform);

		SetHighColor(fFill);
		FillRect(BRect(8, 8, 48, 38));

		SetHighColor(fStroke);
		StrokeRect(BRect(8, 48, 48, 68));

		// A second figure further from the view origin: the displacement a lost
		// offset produces is a *translation of the whole view*, so two figures
		// at different distances from the origin must move together. A bug that
		// merely scaled wrongly would move them apart.
		SetHighColor(fStroke);
		FillRect(BRect(8, 78, 28, 88));
	}

private:
	rgb_color	fFill;
	rgb_color	fStroke;
};


int
main()
{
	BApplication app(kAppSignature);

	// Fixed frame, fixed sizes, no resizing: every coordinate in the capture has
	// to be reproducible across two separate connections.
	BWindow* window = new BWindow(BRect(120, 90, 560, 400),
		"remote_offsets", B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_QUIT_ON_WINDOW_CLOSE);

	window->AddChild(new TransformView(BRect(10, 10, 190, 280), "left",
		(rgb_color){ 220, 40, 40, 255 }, (rgb_color){ 20, 20, 160, 255 }));
	window->AddChild(new TransformView(BRect(220, 10, 420, 280), "right",
		(rgb_color){ 40, 160, 40, 255 }, (rgb_color){ 160, 100, 20, 255 }));

	window->Show();
	app.Run();
	return 0;
}
