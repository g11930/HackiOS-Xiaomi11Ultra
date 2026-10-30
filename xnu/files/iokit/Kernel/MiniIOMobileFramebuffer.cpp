/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <stdarg.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_phase_us=N: how far round the refresh a frame's turn is moved, in microseconds.
 * The panel scans this memory on its own clock and nothing tells us where in its frame we are:
 * a frame written while the panel is scanning it is torn; one written inside its blanking is
 * not. So with fb_beat (the even pace) keeping a fixed grid, this puts the wait N us later than
 * the grid -- the whole frame then falls where it is wanted. 0 (the default): as it was.
 */
static TUNABLE(uint32_t, gPhaseUs, "fb_phase_us", 0);
/*
 * Boot-arg fb_phase_sweep=MS: that same phase, walked round the whole refresh -- one step every MS
 * milliseconds, fb_phase_steps steps (16) to the cycle, over and over -- so that the one place where
 * a frame does not tear is found in one boot instead of one boot a guess. The step is drawn on the
 * panel (fb_phase_show), because the only thing that can see a tear is an eye.
 */
static TUNABLE(uint32_t, gPhaseSweepMs, "fb_phase_sweep", 0);
static TUNABLE(uint32_t, gPhaseSteps, "fb_phase_steps", 16);
static TUNABLE(uint32_t, gPhaseShow, "fb_phase_show", 0);
static uint64_t gSweepBegan;            /* (the frame the walk started on) */
static uint32_t gSweepStep = ~0u;       /* (the step the last frame was written in) */
static uint64_t gSweepPhaseUs;          /* (how far round the refresh that was, in microseconds) */
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/redwood_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);
/* Boot-arg fb_rows=1: compose each row in cache and write it to the panel in one go (see drawRows()). */
static TUNABLE(uint32_t, gRows, "fb_rows", 0);
/*
 * Boot-arg fb_blink=1: which write points the panel at a screen (see blinkThread()). One step at a time,
 * each named on the panel; the last name that stops moving is the write that stops the phone.
 */
static TUNABLE(uint32_t, gBlink, "fb_blink", 0);
#define kBlinkPipes 4
static uint8_t *  fBlinkScreen;         /* (the second screen, filled with magenta) */
static uint32_t   fBlinkBytes;          /* (its size: one screen) */
static uint32_t   fBlinkPhys[2];        /* (the boot screen, then the second one) */
static vm_offset_t fBlinkAt[kBlinkPipes];	/* (the pages of the pipes that fetch the boot screen) */
static unsigned   fBlinkPipesFound;
static volatile uint32_t fBlinkStep;    /* (how far it has got: drawn on the panel) */
static volatile uint32_t fBlinkHeart;   /* (moves every frame: a stopped heart is a stopped kernel) */
/*
 * Boot-arg fb_flip=1: two screens, and the panel pointed at one while the next frame is drawn in the
 * other (see flipPrepare()). fb_flip_us=N: how long after the frame's beat the address is written.
 * fb_flip_show=1: the count and the wait, on the panel.
 */
static TUNABLE(uint32_t, gFlip, "fb_flip", 0);
static TUNABLE(uint32_t, gFlipUs, "fb_flip_us", 0);
static TUNABLE(uint32_t, gFlipShow, "fb_flip_show", 0);
/* (the flush that latches the new address: which bits, and on which of the three controls -- fb_blink's
 * (1<<0)|(1<<1) is the two pipes that fetch this screen, and every bit is the default) */
static TUNABLE(uint32_t, gFlipFlush, "fb_flip_flush", 0xffffffff);
static TUNABLE(uint32_t, gFlipCtl, "fb_flip_ctl", 7);
/*
 * fb_flip_every=N: draw N frames, flip once. 1 (the default) is a flip a frame; higher says how much of
 * the time between two flips the flipping itself costs, by taking it out of the count.
 */
static TUNABLE(uint32_t, gFlipEvery, "fb_flip_every", 1);
/*
 * fb_flip_edge=1: the address is written at the panel's own frame boundary, waited for, not guessed --
 * the interface counts frames, and the counter stepping is the top of a frame, where a flip is not seen
 * by the scan. fb_flip_edge_ms=N: how long to wait for it before flipping anyway. fb_flip_lead=N: how
 * many microseconds after that boundary to write (0: at once, still inside the blanking).
 */
static TUNABLE(uint32_t, gFlipEdge, "fb_flip_edge", 1);
static TUNABLE(uint32_t, gFlipEdgeMs, "fb_flip_edge_ms", 30);
static TUNABLE(uint32_t, gFlipLead, "fb_flip_lead", 0);
/*
 * The panel is two halves on two interfaces (dual DSI), each with a timing engine of its own. The frame
 * boundary that matters is the one of the engine scanning, and if the two are not in step a flip that
 * waits on one lands in the middle of the other -- which is a tear down the middle of the picture, or a
 * band of one frame over another. Both are waited on; fb_flip_intf_sel picks which (0: the first, 1:
 * the second, 2: both). fb_flip_intf_b is the second engine's offset from the display block.
 */
static TUNABLE(uint32_t, gFlipIntfSel, "fb_flip_intf_sel", 0);
static TUNABLE(uint32_t, gFlipIntfB, "fb_flip_intf_b", 0x35000);
/*
 * fb_flip_beat=N: every N milliseconds write a small ticking square straight into the screen the panel
 * is scanning. It needs no lock, no flip and no cooperation from anything else, so it answers one
 * question that a frozen picture otherwise cannot: is the kernel still running? A square that keeps
 * moving while the picture does not is a compositor or a wait that has stopped; a square that stops
 * too is a kernel that is gone, or a display block that is.
 */
static TUNABLE(uint32_t, gFlipBeat, "fb_flip_beat", 1000);
static uint8_t *  fFlipScreen[2];       /* (the boot screen, then the one drawn in while it is scanned) */
static uint32_t   fFlipPhys[2];
static uint32_t   fFlipBytes;
static vm_offset_t fFlipAt[kBlinkPipes];	/* (the pipes that fetch the boot screen) */
static vm_offset_t fFlipCtl[3];         /* (the controls that latch what those pipes were told) */
static unsigned   fFlipPipes;
static uint32_t   fFlipFront;           /* (which screen the panel is on) */
static uint32_t   fFlipCount;           /* (flips written) */
static uint32_t   fFlipWaitUs;          /* (how far into the refresh the last flip was written) */
static uint32_t   fFlipDrawn;           /* (frames drawn since the last flip) */
static uint32_t   fFlipTgpu, fFlipTdraw, fFlipTflip;	/* (us: waiting for the GPU, drawing, flipping) */
static vm_offset_t fFlipIntf;           /* (the interface the panel hangs on: its frame counter) */
static vm_offset_t fFlipIntf2;          /* (the second half's: dual DSI has two timing engines) */
static bool       fFlipIntfOn[2];       /* (each one's line counter was seen moving) */
static uint32_t   fFlipLineB;           /* (where the other half's scan was when the flip went in) */
static uint32_t   fFlipEdgeUs;          /* (us waited for the frame boundary) */
static uint32_t   fFlipLineAt;          /* (the scan line the boundary was caught on) */
static uint32_t   fFlipEdgeMiss;        /* (times the frame counter did not step in time) */
static uint32_t   fFlipEdgeKind;        /* (how the last boundary was caught: 2 in blanking, 3 idle/scanless, 4 skipped) */
static uint32_t   fFlipLineMax;         /* (highest scan line seen; its range is active + blanking, so the blanking window is the slice above the panel's height) */
static uint32_t   fFlipPanelH;          /* (the panel's active height, copied from fPanelHeight so the free flip helpers can see it) */
static uint32_t   fFlipPanelW;          /* (the panel's width, likewise) */
static uint32_t   fFlipRowBytes;        /* (a screen row's length in bytes, likewise) */
static uint32_t   fBeatTick;            /* (squares drawn by the beacon: see beaconThread()) */
static uint32_t   fFlipLineBad;         /* (reads that were not a scan line at all: an all-ones read of a quiet bus) */
static uint64_t   fFlipLastAt;          /* (when the last frame was handed to the panel) */
static uint32_t   fFlipStall;           /* (frames presented without waiting, because the wait had stopped them) */
static uint32_t   fFlipSlow;            /* (frames that sat more than 200 ms between being drawn and being handed over) */
static bool       fFlipArmed;
/* Boot-arg dpu_probe=1: what the display block the firmware left running is doing (see dpuProbe()). */
static bool gHoldFrames;
static void dpuProbeThread(void * self, wait_result_t result);
static void dpuProbe(void);
static void dpuTestThread(void * self, wait_result_t result);
/* Boot-arg te_probe=1: the panel's TE pin, read and not configured (see teProbeThread()). */
static void teProbeThread(void * self, wait_result_t result);

/* Which register points the panel at a screen: see blinkThread() at the end of this file. */
static void blinkThread(void * self, wait_result_t result);
/* fb_flip_beat: a square that ticks in the screen being scanned, so a frozen picture can be told from a
 * frozen kernel -- see beaconThread(). */
static void beaconThread(void * self, wait_result_t result);
/* Two screens and a flip: see flipPrepare() at the end of this file. */
static void flipPrepare(void);
static void flipPresent(uint32_t back, uint64_t began, uint64_t drawn);
/* The flip is tried with writes only: see dpuTestThread() at the end of this file. */
static uint32_t dpuRd(vm_offset_t address);
static void dpuWr(vm_offset_t address, uint32_t value);
static void dpuDrawText(uint8_t * base, uint32_t rowBytes, uint32_t panelWidth, int x0, int y0, const char * text,
    int scale, uint32_t colour);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
/* (ml_io_map_wcomb goes through the IO submap, which is 8 MB and shared: a screen does not fit in it, and the
 * allocation there is KMA_NOFAIL, so asking for one stops the kernel. This one is out of kernel_map, which is
 * address space and nothing else -- it is what video_console.c maps the screen with, for the same reason.) */
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern "C" vm_offset_t ml_vtophys(vm_offset_t vaddr);
#define IO_WIMG_WCOMB           6       /* VM_WIMG_WCOMB: not cacheable, coherent (osfmk/arm/memory_types.h) */

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	uint32_t *      fOutRow;                /* ... and a row of the panel composed in cache (fPanelWidth pixels) */
	uint32_t *      fHelperOut[8];         /* (the same, one for each helper: no two drawers share a row) */
	uint32_t        fHelperStuck;           /* (frames where a helper never said it was done: the rows were taken over here) */
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow, uint32_t * outRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fBlinkPhys[0] = (uint32_t) vinfo.v_physaddr;
	if (fBlinkPhys[0] == 0) {
		fBlinkPhys[0] = 0xea600000;     /* (the loader's splash_region, where the screen is) */
	}
	if (gBlink != 0) {
		/* One run of pages, asked for early while the free memory is still one long run: the display
		 * block fetches it with no mapping of its own (star.dtb gives mdss_mdp no iommus).
		 * KMA_NOPAGEWAIT, so a failure is answered with a log line, never with a wait. */
		vm_size_t bytes = (vm_size_t) fRowBytes * fPanelHeight;
		vm_offset_t va = 0;

		fBlinkBytes = (uint32_t) bytes;
		if (kmem_alloc_contig(kernel_map, &va, bytes, 0, 0, 0, KMA_NOPAGEWAIT,
		    VM_KERN_MEMORY_OSFMK) == KERN_SUCCESS) {
			fBlinkPhys[1] = (uint32_t) ml_vtophys(va);
			fBlinkScreen = (uint8_t *) ml_io_map_unmappable(fBlinkPhys[1], bytes, IO_WIMG_WCOMB);
			IOLog("MiniIOMobileFramebuffer: blink: a second screen at %#x, the first at %#x\n",
			    fBlinkPhys[1], fBlinkPhys[0]);
		} else {
			IOLog("MiniIOMobileFramebuffer: blink: no second screen, %u bytes contiguous\n",
			    (uint32_t) bytes);
		}
	}
	if (gFlip != 0) {
		/* One screen more than the loader left room for, asked for early while the free memory is
		 * still one long run of pages. (KMA_NOPAGEWAIT: a failure is a log line, never a wait.)
		 * Two screens of the same size and the same stride, so that the pipes fetching one can be
		 * pointed at the other and nothing else in them has to be told. */
		vm_size_t bytes = (vm_size_t) fRowBytes * fPanelHeight;
		vm_offset_t va = 0;

		fFlipPhys[0] = fBlinkPhys[0];
		fFlipScreen[0] = (uint8_t *) vinfo.v_baseaddr;
		fFlipBytes = (uint32_t) bytes;
		if (kmem_alloc_contig(kernel_map, &va, bytes, 0, 0, 0, KMA_NOPAGEWAIT,
		    VM_KERN_MEMORY_OSFMK) == KERN_SUCCESS) {
			fFlipPhys[1] = (uint32_t) ml_vtophys(va);
			fFlipScreen[1] = (uint8_t *) ml_io_map_unmappable(fFlipPhys[1], bytes, IO_WIMG_WCOMB);
			IOLog("MiniIOMobileFramebuffer: flip: two screens, %#x and %#x (%u bytes each)\n",
			    fFlipPhys[0], fFlipPhys[1], (uint32_t) bytes);
			if (fFlipScreen[1] != NULL) {
				/* (the second one starts as a copy of the first: a corner the layers never
				 * touch is then the last frame's corner, not whatever the pages held) */
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					memcpy(fFlipScreen[1] + (size_t) y * fRowBytes,
					    fFlipScreen[0] + (size_t) y * fRowBytes, fRowBytes);
				}
				__asm__ volatile ("dsb sy" ::: "memory");
			}
		} else {
			IOLog("MiniIOMobileFramebuffer: flip: no second screen, %u bytes contiguous\n",
			    (uint32_t) bytes);
		}
	}
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	/* a row to compose in cache, for every one who draws rows (boot-arg fb_rows=1: off by default,
	 * because a frame drawn through one has stopped this kernel where a frame drawn in place did not) */
	if (gRows != 0) {
	fOutRow = IONew(uint32_t, fPanelWidth);
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		fHelperOut[i] = IONew(uint32_t, fPanelWidth);
	}
	if (fOutRow == NULL) {
		IOLog("MiniIOMobileFramebuffer: no row to compose frames in: the panel is written where it stands\\n");
	}
	}

	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	{
		uint32_t test = 0;
		thread_t testThread;

		if (PE_parse_boot_argn("dpu_test", &test, sizeof(test)) && test != 0 &&
		    kernel_thread_start(&dpuTestThread, NULL, &testThread) == KERN_SUCCESS) {
			thread_set_thread_name(testThread, "the panel: a second screen, by writing only");
			thread_deallocate(testThread);
		}
	}
	{
		thread_t blinkT;

		if (gBlink != 0 && kernel_thread_start(&blinkThread, NULL, &blinkT) == KERN_SUCCESS) {
			thread_set_thread_name(blinkT, "the panel: which write");
			thread_deallocate(blinkT);
		}
	}
	{
		thread_t beatT;

		if (gFlipBeat != 0 && kernel_thread_start(&beaconThread, NULL, &beatT) == KERN_SUCCESS) {
			thread_set_thread_name(beatT, "the panel: a square that ticks");
			thread_deallocate(beatT);
		}
	}
	{
		uint32_t te = 0;
		thread_t teThread;

		if (PE_parse_boot_argn("te_probe", &te, sizeof(te)) && te != 0 &&
		    kernel_thread_start(&teProbeThread, NULL, &teThread) == KERN_SUCCESS) {
			thread_set_thread_name(teThread, "the panel: its TE pin");
			thread_deallocate(teThread);
		}
	}
	{
		uint32_t probe = 0;
		thread_t probeThread;

		if (PE_parse_boot_argn("dpu_probe", &probe, sizeof(probe)) && probe != 0 &&
		    kernel_thread_start(&dpuProbeThread, NULL, &probeThread) == KERN_SUCCESS) {
			thread_set_thread_name(probeThread, "the display block: what it is doing");
			thread_deallocate(probeThread);
		}
	}
	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("RedwoodWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	uint32_t        generation;     /* (which frame this job belongs to: a helper that ran late counts its own, not the next) */
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	bool            filter, unscaled;
};

/*
 * The job is kept here, not on the stack of the frame that made it. A helper that the scheduler left for
 * more than the wait's worth of milliseconds used to read a RowJob whose frame had already returned -- a
 * pointer, a stride and a surface pointer out of a stack that had since been used for something else, and
 * a load through the pixels pointer is how that ends: a data abort. A ring, so the slot being read is not
 * the one being filled; eight frames at 60 a second is a tenth of a second of slack for a late helper.
 */
#define kJobRing 8
static RowJob   fJobRing[kJobRing];
static unsigned fJobRingAt;

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow,
    uint32_t * outRow)
{
	/* (without a row of its own a drawer writes the panel where it stands: as it did before there was one) */
	const bool buffered = outRow != NULL;
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;
		uint8_t * panel;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		panel = fBase + (size_t) y * fRowBytes;
		to = buffered ? outRow : (uint32_t *) panel;
		if (job->unscaled) {
			/* not scaled at all: the row goes across in one copy. (It used to be two -- into the row's
			 * own buffer and then into the panel -- and the panel's memory is not cached, so the second
			 * copy was the half of a frame's cost that bought nothing) */
			const uint8_t * from = pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4;

			if (buffered) {
				memcpy(panel + (size_t) px0 * 4, from, (size_t) sw * 4);
			} else {
				memcpy(to + px0, from, (size_t) sw * 4);
			}
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (((const uint32_t *) panel)[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
		if (buffered && cx1 > cx0) {
			/* one row at a time, out of cache: the panel's memory is not cached, and a store of its own for
			 * every one of 4.6 million pixels is the whole of what a frame costs. (cx1 > cx0: a layer that is
			 * off the right of the panel has cx0 past cx1, and a length worked out from that is no length) */
			memcpy(panel + (size_t) cx0 * 4, outRow + cx0, (size_t) (cx1 - cx0) * 4);
		}
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;
		uint32_t gen;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		gen = (uint32_t) done;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->py1 - job->py0;
		y0 = job->py0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->py0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend, fb->fHelperOut[index]);
		IOLockLock(fb->fHelperLock);
		/* (the frame may have been taken over here already, when this helper was late: it no longer owns
		 * a share of this count, and the count may by now belong to the frame after -- so only a helper
		 * whose own generation is still the current one counts down) */
		if (fb->fHelperGeneration == gen && fb->fHelpersBusy != 0) {
			fb->fHelpersBusy--;
			if (fb->fHelpersBusy == 0) {
				IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
			}
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	/* (a slot that outlives this call: a helper still reading it after the return reads memory of its own
	 * frame, not whatever the stack has been reused for --> */
	RowJob * job = &fJobRing[fJobRingAt++ % kJobRing];

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0 > (int32_t) fPanelWidth ? (int32_t) fPanelWidth : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1 < 0 ? 0 : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job->pixels = s->pixels();
	job->stride = s->info.bytesPerRow;
	job->sx = sx;
	job->sy = sy;
	job->sw = sw;
	job->sh = sh;
	job->px0 = px0;
	job->py0 = py0;
	job->px1 = px1;
	job->py1 = py1;
	job->cx0 = cx0;
	job->cx1 = cx1;
	job->filter = filter;
	job->unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(job, py0, py1, fBlendRow, fOutRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	IOLockLock(fHelperLock);
	fHelperGeneration++;
	job->generation = fHelperGeneration;
	fHelperJob = job;
	fHelpersBusy = fThreads - 1;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(job, py0, py0 + (int32_t) ((int64_t) (py1 - py0) / fThreads), fBlendRow, fOutRow);
	/* (a helper that stops answering would otherwise hold this lock -- and the compositor with it -- for
	 * good, which is a picture that never changes again. So the wait is bounded: after a hundred
	 * milliseconds the rest of the rows are drawn here. Two writers of one row write the same pixels;
	 * a frame that never finishes is not so forgiving) */
	IOLockLock(fHelperLock);
	for (unsigned spins = 0; fHelpersBusy != 0 && spins < 100; spins++) {
		IOLockUnlock(fHelperLock);
		IOSleep(1);
		IOLockLock(fHelperLock);
	}
	if (fHelpersBusy != 0) {
		fHelperStuck++;
		fHelpersBusy = 0;
		IOLockUnlock(fHelperLock);
		if (fHelperStuck <= 4) {
			IOLog("MiniIOMobileFramebuffer: a helper did not finish a share (%u); the rows were taken over here\n",
			    fHelperStuck);
		}
		drawRows(job, py0 + (int32_t) ((int64_t) (py1 - py0) / fThreads), py1, fBlendRow, fOutRow);
		return;
	}
	IOLockUnlock(fHelperLock);
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		redwood_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		if (gHoldFrames) {
			IOSleep(1000);          /* (dpu_probe: the panel is being written on and must keep it) */
			continue;
		}
		show(swap, surfaces);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint64_t entered = began;               /* (when the frame was picked up: the wait for the GPU is from here) */
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (gOnTheBeat) {
		uint64_t period, aim, next, phase = 0;
		uint32_t step = 0;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		if (gPhaseSweepMs != 0) {
			/* the walk: which step of the refresh this frame is to be written in */
			uint32_t steps = gPhaseSteps >= 2 && gPhaseSteps <= 64 ? gPhaseSteps : 16;

			if (gSweepBegan == 0) {
				gSweepBegan = began;
			}
			step = (uint32_t) ((micros(began - gSweepBegan) / gPhaseSweepMs) % steps);
			phase = period * step / steps;
			gSweepPhaseUs = micros(phase);
			if (step != gSweepStep) {
				gSweepStep = step;
				IOLog("fb: phase %u of %u: %llu us round the refresh\n", step, steps, gSweepPhaseUs);
			}
		} else if (gPhaseUs != 0) {
			uint64_t us = (uint64_t) gPhaseUs * NSEC_PER_USEC;

			nanoseconds_to_absolutetime(us, &phase);
			gSweepPhaseUs = gPhaseUs;
			gSweepStep = ~0u;
		}
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next + phase < began; next += period) {
			}
			/* (with a phase given, the grid is left alone: the nudge below is what would eat it) */
			if (phase == 0 && next - began > aim) {
				next -= (next - began - aim) / 8;
			} else if (phase == 0) {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next + phase);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	/* (which screen this frame is drawn in: the one the panel is not on, so that nothing is ever written
	 * where the panel is scanning -- see flipPrepare()) */
	uint32_t flipBack = 0;
	uint8_t * const flipWas = fBase;
	bool flipNow = false;

	if (gFlip != 0 && fFlipScreen[1] != NULL) {
		fFlipPanelH = fPanelHeight;     /* (copied for the free flip helpers) */
		fFlipPanelW = fPanelWidth;
		fFlipRowBytes = fRowBytes;
		if (!fFlipArmed) {
			flipPrepare();
		}
		if (fFlipArmed) {
			/* (fb_flip_every: the frame goes into the other screen only on the frames that are flipped,
			 * so that what the flipping costs is not paid on every one) */
			fFlipDrawn++;
			flipNow = fFlipDrawn >= (gFlipEvery >= 1 ? gFlipEvery : 1);
			flipBack = flipNow ? (fFlipFront ^ 1) : fFlipFront;
			fBase = fFlipScreen[flipBack];
		}
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			drawLayer(s, src, dst);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	drawn = mach_absolute_time();
	if (fBase != NULL && gBlink != 0) {
		char line[64];

		fBlinkHeart++;
		snprintf(line, sizeof(line), "%X %X %X %08X", fBlinkStep, fBlinkPipesFound, fBlinkHeart,
		    fBlinkPhys[1]);
		dpuDrawText(fBase, fRowBytes, fPanelWidth, 20, 40, line, 4, 0x00ff00ff);
	}
	if ((gPhaseShow != 0 || gPhaseSweepMs != 0) && fBase != NULL) {
		/* (drawn after the frame, so it is the last thing written: two digits, big, out of the way) */
		char line[24];
		uint32_t at = (uint32_t) (fPanelHeight > 500 ? fPanelHeight - 420 : fPanelHeight / 2);

		snprintf(line, sizeof(line), "%02u:%04u", gSweepStep == ~0u ? 0 : gSweepStep % 100,
		    (uint32_t) (gSweepPhaseUs / 100) % 10000);
		dpuDrawText(fBase, fRowBytes, fPanelWidth, 40, (int) at, line, 8, 0x00ffffff);
	}
	if (gFlip != 0 && fFlipArmed && flipNow) {
		uint64_t done;

		fFlipTgpu = (uint32_t) micros(ready - entered);
		fFlipTdraw = (uint32_t) micros(drawn - ready);
		if (gFlipShow != 0) {
			/* (drawn into the frame just finished, so it goes up with it) */
			char line[48];
			/* one colour a frame, at the top and at the bottom: if these two ever disagree the picture
			 * on the panel is two frames, and which of them is the stale one says where the flip went */
			uint32_t tag = (fFlipCount & 1) ? 0x0000ffff : 0x00ff00ff;

			for (uint32_t dy = 0; dy < 12; dy++) {
				for (unsigned e = 0; e < 2; e++) {
					uint32_t y = e == 0 ? dy : fPanelHeight - 1 - dy;
					uint32_t * to = (uint32_t *) (fBase + (size_t) y * fRowBytes);

					for (uint32_t x = 0; x < fPanelWidth; x++) {
						to[x] = tag;
					}
				}
			}
			/* (a black plate at the bottom so the numbers are readable over whatever the UI shows,
			 * kept clear of the 12-px frame-tag bar just above it) */
			for (uint32_t yb = fPanelHeight - 210; yb < fPanelHeight - 64; yb++) {
				uint32_t * to = (uint32_t *) (fBase + (size_t) yb * fRowBytes);

				for (uint32_t x = 0; x < fPanelWidth; x++) {
					to[x] = 0x00000000;
				}
			}
			snprintf(line, sizeof(line), "%u %u %u", fFlipCount, flipBack, fFlipTgpu);
			dpuDrawText(fBase, fRowBytes, fPanelWidth, 20, fPanelHeight - 196, line, 4, 0x00ffff00);
			snprintf(line, sizeof(line), "%u %u %u", fFlipTdraw, fFlipTflip, fFlipWaitUs);
			dpuDrawText(fBase, fRowBytes, fPanelWidth, 20, fPanelHeight - 156, line, 4, 0x00ffff00);
			/* (us waited, the scan line at the end of the wait, and how it ended: 2 in blanking,
			 * 3 idle/scanless, 5 blanking never came so the frame went in anyway, 6 the wait itself
			 * took half a second -- that one is presented at once) */
			snprintf(line, sizeof(line), "%u %u %u", fFlipEdgeUs, fFlipLineAt, fFlipEdgeKind);
			dpuDrawText(fBase, fRowBytes, fPanelWidth, 20, fPanelHeight - 116, line, 4, 0x00ff8000);
			/* (misses, reads that were not scan lines, frames presented without waiting -- then the
			 * address the panel is on, which must equal the one the pipes were given) */
			snprintf(line, sizeof(line), "%u %u %u %x", fFlipEdgeMiss, fFlipLineBad, fFlipStall,
			    fFlipPhys[fFlipFront]);
			dpuDrawText(fBase, fRowBytes, fPanelWidth, 20, fPanelHeight - 76, line, 4, 0x00ff8000);
		}
		flipPresent(flipBack, began, drawn);
		done = mach_absolute_time();
		fFlipTflip = (uint32_t) micros(done - drawn);
		fFlipDrawn = 0;
		fBase = flipWas;
	}
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		redwood_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		fStatSwapsLate = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
	} else {
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		IOLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gTouchLat) {
			redwood_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.ios, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}

/* ------------------------------------------------------------------------------------------------------------
 * Boot-arg dpu_probe=1: what the display block the firmware left running is doing, and where a second screen
 * of memory would go. Nothing is written to the display block: the only writes are the two clock enables
 * redwood_adreno.c's display_probe makes (the display's bus clock in GCC, and the register bus in DISPCC).
 * The point: the panel is a DSI video panel that the firmware set going, so its frame is fetched by one of
 * the display block's layer pipes (SSPP) out of one piece of memory, over and over, at the panel's own rate.
 * This finds that pipe -- the register that holds the address of the boot framebuffer -- because a frame that
 * must not tear has to be shown by pointing the pipe at the other of two screens at a frame boundary.
 * --------------------------------------------------------------------------------------------------------- */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define DPU_PHYS                0x0ae00000
#define DPU_LEN                 0x00084000
#define DISPCC_PHYS             0x0af00000
#define GCC_PHYS                0x00100000
#define GCC_DISP_AHB_CBCR       0x27004
#define DISPCC_MDSS_CORE_GDSCR  0x1004
#define DISPCC_MDSS_AHB_CBCR    0x207c          /* (dispcc-sm8350.c: SM8350's, not SM8250's 0x1050) */
#define DPU_CBCR_ENABLE         (1u << 0)
#define DPU_CBCR_OFF            (1u << 31)

static uint32_t __attribute__((noinline))
dpuRd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
dpuWr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

/* A 5 x 7 font for the sixteen hexadecimal digits, the colon and the space: enough to write numbers on the
 * panel where the host can photograph them (see dpuProbe(): the loader's log is not the kernel's). */
static const uint8_t dpuGlyph[18][5] = {
	{ 0x3E, 0x51, 0x49, 0x45, 0x3E },       /* 0 */
	{ 0x00, 0x42, 0x7F, 0x40, 0x00 },       /* 1 */
	{ 0x42, 0x61, 0x51, 0x49, 0x46 },       /* 2 */
	{ 0x21, 0x41, 0x45, 0x4B, 0x31 },       /* 3 */
	{ 0x18, 0x14, 0x12, 0x7F, 0x10 },       /* 4 */
	{ 0x27, 0x45, 0x45, 0x45, 0x39 },       /* 5 */
	{ 0x3C, 0x4A, 0x49, 0x49, 0x30 },       /* 6 */
	{ 0x01, 0x71, 0x09, 0x05, 0x03 },       /* 7 */
	{ 0x36, 0x49, 0x49, 0x49, 0x36 },       /* 8 */
	{ 0x06, 0x49, 0x49, 0x29, 0x1E },       /* 9 */
	{ 0x7E, 0x11, 0x11, 0x11, 0x7E },       /* A */
	{ 0x7F, 0x49, 0x49, 0x49, 0x36 },       /* B */
	{ 0x3E, 0x41, 0x41, 0x41, 0x22 },       /* C */
	{ 0x7F, 0x41, 0x41, 0x22, 0x1C },       /* D */
	{ 0x7F, 0x49, 0x49, 0x49, 0x41 },       /* E */
	{ 0x7F, 0x09, 0x09, 0x09, 0x01 },       /* F */
	{ 0x00, 0x00, 0x00, 0x00, 0x00 },       /* space */
	{ 0x00, 0x36, 0x36, 0x00, 0x00 },       /* : */
};

static void
dpuDrawText(uint8_t * base, uint32_t rowBytes, uint32_t panelWidth, int x0, int y0, const char * text, int scale,
    uint32_t colour)
{
	int x = x0;

	for (; *text != 0; text++) {
		char c = *text;
		int at = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c == ':' ? 17 : 16;
		const uint8_t * glyph = dpuGlyph[at];

		for (int col = 0; col < 5; col++) {
			for (int row = 0; row < 7; row++) {
				if (glyph[col] & (1 << row)) {
					for (int dy = 0; dy < scale; dy++) {
						uint32_t * to = (uint32_t *) (base + (size_t) (y0 + row * scale + dy) * rowBytes);

						for (int dx = 0; dx < scale; dx++) {
							if ((uint32_t) (x + col * scale + dx) < panelWidth) {
								to[x + col * scale + dx] = colour;
							}
						}
					}
				}
			}
		}
		x += 6 * scale;
	}
}

/* Four registers a line, and the one that holds the framebuffer's address marked. */
static void
dpuDump(const char * what, vm_offset_t base, uint32_t from, uint32_t to, uint32_t fbPhys)
{
	for (uint32_t at = from; at < to; at += 16) {
		char line[192];
		int n = snprintf(line, sizeof(line), "DPU %s +%03x:", what, at);

		for (uint32_t i = 0; i < 16 && at + i < to; i += 4) {
			uint32_t value = dpuRd(base + at + i);

			n += snprintf(line + n, sizeof(line) - (size_t) n, " %08x", value);
			if (value == fbPhys) {
				n += snprintf(line + n, sizeof(line) - (size_t) n, "*FB");
			} else if ((value & 0xfff) == 0 && value >= 0x40000000 && value < 0xf0000000) {
				n += snprintf(line + n, sizeof(line) - (size_t) n, "*");
			}

		}
		IOLog("%s\n", line);
	}
}

/* Every so often, again: the kernel's log keeps only the last of it, and this has to be in there when it is
 * read out (the loader's `oem log`), seconds or minutes after these lines were first written. */
static void
dpuProbeAgain(void * self __unused, wait_result_t result __unused)
{
	static const uint32_t vigAt[2] = { 0x50000, 0x70000 };
	vm_offset_t dpu = ml_io_map(DPU_PHYS, DPU_LEN);
	unsigned round = 0;

	for (;;) {
		IOSleep(6000);
		round++;
		for (unsigned i = 0; i < 2; i++) {
			vm_offset_t at = dpu + vigAt[i];
			uint32_t src = dpuRd(at + 0x000), stride = dpuRd(at + 0x010), size = dpuRd(at + 0x038);
			uint32_t xy = dpuRd(at + 0x03c), fmt = dpuRd(at + 0x040), op = dpuRd(at + 0x030);

			if (src != 0 || stride != 0 || size != 0) {
				IOLog("DPU %u: vig%u src %#x stride %#x size %#x xy %#x format %#x opmode %#x\n", round, i, src,
				    stride, size, xy, fmt, op);
			}
		}
		IOLog("DPU %u: ctl0 %#x %#x %#x %#x %#x %#x %#x %#x, intf1 line %u frame %u\n", round,
		    dpuRd(dpu + 0x16000), dpuRd(dpu + 0x16004), dpuRd(dpu + 0x16008), dpuRd(dpu + 0x1600c),
		    dpuRd(dpu + 0x16010), dpuRd(dpu + 0x16014), dpuRd(dpu + 0x16018), dpuRd(dpu + 0x1601c),
		    dpuRd(dpu + 0x360b0), dpuRd(dpu + 0x360ac));
	}
}

/* Late: the clocks the reading needs are the ones the system's own drivers have turned on by then. */
static void
dpuProbeThread(void * self __unused, wait_result_t result __unused)
{
	IOSleep(45000);
	gHoldFrames = true;             /* (the presenter stops: the panel keeps what is drawn on it here) */
	IOSleep(300);
	dpuProbe();                     /* (returns only if the boot arg went away: otherwise it never does) */
}

/* A numbered line on the panel, 8 lines down and 30 a line: the numbers are the whole of the log, so every
 * line carries its own number and what it means is written down where the line is written. */
static int dpuLogLine;

static void
dpuLog(const char * format, ...)
{
	char body[128], line[160];
	va_list ap;
	int n = dpuLogLine++;

	va_start(ap, format);
	vsnprintf(body, sizeof(body), format, ap);
	va_end(ap);
	snprintf(line, sizeof(line), "%02X %s", n, body);
	dpuDrawText((uint8_t *) vinfo.v_baseaddr, vinfo.v_rowbytes, vinfo.v_width, 8, 8 + n * 30, line, 2, 0x00ffffff);
}

/*
 * Boot-arg dpu_probe=1: what the display block the firmware left running is doing, and where a second screen
 * of memory would go. Everything found is written on the panel itself -- the loader hands back its own log,
 * not this kernel's -- a numbered line at a time, each line before the read it is about, so the last line
 * says where the reading stopped.
 *
 * Nothing of the block is read before the three checks below. The block's registers have a bus clock of their
 * own, the firmware took it away when it had the panel going, and a register read without that clock does not
 * answer: it stops the phone. redwood_adreno.c's display_probe makes the same three checks on this phone.
 */
#define INTF_PHYS                0x0ae36000     /* dt: dsi-ctrl0's mdp_intf_base */
#define CTL0_PHYS                0x0ae16000     /* dt: mdss_mdp ctl-off to ctl-off + 0x5000, first of them */
#define MIX0_PHYS                0x0ae45000     /* dt: the mixers, first of them */
#define INTF_TIMING_ENGINE_EN    0x000
#define INTF_HSYNC_CTL           0x008          /* clocks a line << 16 | of which the pulse */
#define INTF_VSYNC_PERIOD        0x00c          /* clocks a frame */
#define INTF_DISPLAY_V_START     0x01c
#define INTF_DISPLAY_V_END       0x024
#define INTF_HCTL                0x03c
#define INTF_FRAME_LINE_COUNT_EN 0x0a8
#define INTF_FRAME_COUNT         0x0ac          /* (counts although the enable above reads 0) */
#define INTF_LINE_COUNT          0x0b0
#define DPU_GDSC_ON              (1u << 31)

/* Four registers a line, the offset first, so that a value found at a known offset can be named. */
static void
dpuLogRegs(unsigned tag, vm_offset_t base, uint32_t from, uint32_t to)
{
	for (uint32_t at = from; at < to; at += 16) {
		dpuLog("%X %03X %08X %08X %08X %08X", tag, at, dpuRd(base + at), dpuRd(base + at + 4),
		    dpuRd(base + at + 8), dpuRd(base + at + 0xc));
	}
}

/* A check failed: nothing of the block can be read, so nothing is. The line number says which check. */
static void
dpuProbeStop(void)
{
	for (;;) {
		IOSleep(1000);
	}
}

static void
dpuProbe(void)
{
	uint32_t on = 0, fbPhys, clock;
	vm_offset_t gcc, dispcc, at;
	static const uint32_t vigAt[4] = { 0x5000, 0x7000, 0x9000, 0xb000 };	/* (star.dtb: sde-sspp-off) */

	if (!PE_parse_boot_argn("dpu_probe", &on, sizeof(on)) || on == 0) {
		return;
	}
	fbPhys = (uint32_t) vinfo.v_physaddr;
	if (fbPhys == 0) {
		fbPhys = 0xea600000;            /* the loader's splash_region, where the screen is */
	}
	dpuLog("FB %08X W %X RB %X", fbPhys, vinfo.v_width, vinfo.v_rowbytes);

	/* 1: the bus clock of the display's clock controller, in GCC (this one answers either way) */
	gcc = ml_io_map(GCC_PHYS, 0x80000);
	on = dpuRd(gcc + GCC_DISP_AHB_CBCR);
	dpuLog("GCC %08X", on);
	if ((on & DPU_CBCR_OFF) || !(on & DPU_CBCR_ENABLE)) {
		dpuWr(gcc + GCC_DISP_AHB_CBCR, on | DPU_CBCR_ENABLE);
		IOSleep(10);
		on = dpuRd(gcc + GCC_DISP_AHB_CBCR);
		dpuLog("GCC %08X", on);
	}
	if (!(on & DPU_CBCR_ENABLE)) {
		dpuLog("STOP AT 4");
		dpuProbeStop();
	}
	/* 2: the block's power switch, in its clock controller: read and written down, but no reason to stop -- on this
	 * phone it reads 0x1f with the panel being scanned, so the bit that means "on" on the phone it was measured on
	 * means nothing here, and stopping on it stops the probe for nothing */
	/* (0x8000, not 0x2000: on SM8350 the clock of the block's register bus is at 0x207c, past the end of a
	 * 8 KB mapping -- a read of an address outside the mapping is not a read of the register, it is a fault) */
	dispcc = ml_io_map(DISPCC_PHYS, 0x8000);
	on = dpuRd(dispcc + DISPCC_MDSS_CORE_GDSCR);
	dpuLog("GDSC %08X", on);
	/* 3: the bus clock of the block's own registers */
	clock = dpuRd(dispcc + DISPCC_MDSS_AHB_CBCR);
	dpuLog("CAHB %08X", clock);
	if (clock & DPU_CBCR_OFF) {
		dpuWr(dispcc + DISPCC_MDSS_AHB_CBCR, clock | DPU_CBCR_ENABLE);
		IOSleep(10);
		clock = dpuRd(dispcc + DISPCC_MDSS_AHB_CBCR);
		dpuLog("CAHB %08X", clock);
	}
	if (clock & DPU_CBCR_OFF) {
		/* (the bus clock is still halted: a read of the block now would stop the phone, so there is none) */
		dpuLog("STOP AT 9");
		dpuProbeStop();
	}
	dpuLog("BUS %08X", clock);
	/* the interface the panel hangs on: its timing, and the two counters that say where the scan is */
	at = ml_io_map(INTF_PHYS, 0x1000);
	dpuLog("EN %X HS %X VS %X", dpuRd(at + INTF_TIMING_ENGINE_EN), dpuRd(at + INTF_HSYNC_CTL) >> 16,
	    dpuRd(at + INTF_VSYNC_PERIOD));
	dpuLog("V %X %X H %X", dpuRd(at + INTF_DISPLAY_V_START), dpuRd(at + INTF_DISPLAY_V_END), dpuRd(at + INTF_HCTL));
	dpuLog("CEN %X F %X L %X", dpuRd(at + INTF_FRAME_LINE_COUNT_EN), dpuRd(at + INTF_FRAME_COUNT),
	    dpuRd(at + INTF_LINE_COUNT));
	/* the layer pipes, one tag each: the one whose source address is the boot framebuffer is the one to flip */
	for (unsigned i = 0; i < 4; i++) {
		at = ml_io_map(DPU_PHYS + vigAt[i], 0x1000);
		dpuLogRegs(i + 1, at, 0x000, 0x050);
	}
	/* the control that latches what the pipes were told, and the mixer they feed */
	at = ml_io_map(CTL0_PHYS, 0x1000);
	dpuLogRegs(5, at, 0x000, 0x060);
	at = ml_io_map(MIX0_PHYS, 0x1000);
	dpuLogRegs(6, at, 0x000, 0x060);
	for (;;) {
		IOSleep(1000);
	}
}

/* ---- a second screen, by writing only --------------------------------------------------------------- */

/* the layer pipes, at the offsets star.dtb gives for them (sde-sspp-off): four VIG, then four DMA. (These
 * were 0x50000 and up, ten times too far: an offset past every register the block has answers nothing.) */
#define TEST_SSPP0              0x05000
#define TEST_SSPP1              0x07000
#define TEST_SSPP2              0x09000
#define TEST_SSPP3              0x0b000
#define TEST_SSPP4              0x25000
#define TEST_SSPP5              0x27000
#define TEST_SSPP6              0x29000
#define TEST_SSPP7              0x2b000
#define TEST_CTL_FLUSH          0x018           /* dpu_hw_ctl.c */
/* (every layer pipe and mixer bit, and bit 17, which goes with the mixers: dpu_hw_ctl.c's masks) */
/* every bit of the control's flush: whichever pipe is the one fetching the frame, its bit is set, and a
 * bit for a pipe that is not there is a bit that clears nothing */
#define TEST_FLUSH_ALL          0xffffffff
#define TEST_SSPP_SRC0_ADDR     0x000
/* the region the panel is already being scanned out of: the loader's splash_region, 36.7 MB, of which
 * the screen that is in use takes the first 18.4 MB -- the rest is the only memory on this phone that a
 * second screen can be put in without asking the kernel for 18 MB it does not have */
#define SPLASH_PHYS             0xea600000u
#define SPLASH_END              0xec900000u

/* A white bar across the live screen: which of these turn up says how far the thread got, and a panel that
 * has stopped updating still has them. */
static void
dpuMark(int row, int rows)
{
	for (int y = row; y < row + rows && (uint32_t) y < vinfo.v_height; y++) {
		uint32_t * line = (uint32_t *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes);

		for (uint32_t x = 0; x < vinfo.v_width; x++) {
			line[x] = 0x00ffffff;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
dpuTestThread(void * self __unused, wait_result_t result __unused)
{
	uint32_t on = 0, value, delay = 0, bytes, phys1, spin, room;
	uint32_t phys0 = (uint32_t) vinfo.v_physaddr;
	vm_offset_t gcc, dispcc, va1, ctl;
	static const uint32_t ssppAt[8] = { TEST_SSPP0, TEST_SSPP1, TEST_SSPP2, TEST_SSPP3,
	    TEST_SSPP4, TEST_SSPP5, TEST_SSPP6, TEST_SSPP7 };

	if (!PE_parse_boot_argn("dpu_test", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (!PE_parse_boot_argn("dpu_flip_ms", &delay, sizeof(delay)) || delay == 0 || delay > 300000) {
		delay = 30000;
	}
	IOSleep(delay);
	bytes = vinfo.v_rowbytes * vinfo.v_height;
	IOLog("MiniIOMobileFramebuffer: a second screen for %#x (%u bytes): trying it\n", phys0, bytes);
	dpuMark(0, 12);                         /* 1: the thread is running */

	/* the two clocks the block's registers need: reads in these two blocks answer, it is the display block that
	 * does not, and a write of the enable bit is all either of them needs */
	gcc = ml_io_map(GCC_PHYS, 0x80000);
	value = dpuRd(gcc + GCC_DISP_AHB_CBCR);
	dpuWr(gcc + GCC_DISP_AHB_CBCR, value | DPU_CBCR_ENABLE);
	IOSleep(10);
	dispcc = ml_io_map(DISPCC_PHYS, 0x8000);
	value = dpuRd(dispcc + DISPCC_MDSS_AHB_CBCR);
	dpuWr(dispcc + DISPCC_MDSS_AHB_CBCR, value | DPU_CBCR_ENABLE);
	IOSleep(10);
	dpuMark(40, 12);                        /* 2: the clocks are asked for */

	/* the second screen: the rest of the region the panel is already on. (It is not allocated. An allocation of
	 * 18432000 bytes was asked for with KM_SLEEP, and kmem_alloc panics rather than say no when it cannot find
	 * that many pages in one run -- that, and nothing to do with the drawing, is what stopped four kernels.
	 * The region the loader reserved is 36700160 bytes and the screen in use is 18432000 of them, which leaves
	 * 18284544: short of a whole second screen by 147456 bytes, 25.6 rows at the foot of it.) */
	phys1 = phys0 + bytes;
	room = SPLASH_END > phys1 ? SPLASH_END - phys1 : 0;
	if (room < bytes) {
		IOLog("MiniIOMobileFramebuffer: the second screen is %u bytes short of a whole one\n", bytes - room);
	}
	if (room == 0) {
		dpuMark(80, 12);                /* 2a: no room after the screen in use */
		return;
	}
	va1 = ml_io_map_unmappable(phys1, room, IO_WIMG_WCOMB);
	if (va1 == 0) {
		IOLog("MiniIOMobileFramebuffer: no mapping for the second screen\n");
		dpuMark(120, 12);               /* 2b: no mapping */
		return;
	}
	dpuMark(160, 12);                       /* 3: the second screen is there */

	/* a screen of its own, so that the panel showing it cannot be mistaken for the panel still showing the
	 * first one: one flat colour over all of it, and a white band across the middle */
	memset((void *) va1, 0x20, (size_t) room);
	for (uint32_t y = vinfo.v_height / 2 - 80; y < vinfo.v_height / 2 + 80; y++) {
		if ((size_t) y * vinfo.v_rowbytes + vinfo.v_rowbytes <= room) {
			memset((void *) (va1 + (size_t) y * vinfo.v_rowbytes), 0xff, (size_t) vinfo.v_rowbytes);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	/* (read it back: a mapping that is not there reads whatever the old one had, and that is worth knowing
	 * before the pipe is pointed at it) */
	IOLog("MiniIOMobileFramebuffer: the second screen reads back %#x %#x at its middle, %#x at its foot\n",
	    *(volatile uint32_t *) (va1 + (size_t) (vinfo.v_height / 2 - 100) * vinfo.v_rowbytes),
	    *(volatile uint32_t *) (va1 + (size_t) (vinfo.v_height / 2 + 100) * vinfo.v_rowbytes),
	    *(volatile uint32_t *) (va1 + room - 4));
	dpuMark(200, 12);                       /* 4: the second screen has the band the panel is to show */

	/* the pipe is pointed at it, in every pipe the loader is known to use (the one that is not fetching the
	 * frame is not using that register), and then the control is told to latch */
	/* (which pipe is fetching the frame is read out of it rather than assumed: the address each of the eight
	 * holds goes on the screen before and after it is changed, so that a flip which does not happen still says
	 * why. Both screens get it: the second one is the one the panel is being asked to show, and the first is the
	 * one it shows if the asking does not work.) */
	{
		vm_offset_t sspp[8];
		uint32_t before[8], after[8];
		char line[64];

		for (unsigned i = 0; i < 8; i++) {
			sspp[i] = ml_io_map(DPU_PHYS + ssppAt[i], 0x1000);
			before[i] = sspp[i] != 0 ? dpuRd(sspp[i] + TEST_SSPP_SRC0_ADDR) : 0;
		}
		for (unsigned i = 0; i < 8; i++) {
			if (sspp[i] != 0) {
				dpuWr(sspp[i] + TEST_SSPP_SRC0_ADDR, phys1);
			}
			after[i] = sspp[i] != 0 ? dpuRd(sspp[i] + TEST_SSPP_SRC0_ADDR) : 0;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		for (unsigned line4 = 0; line4 < 4; line4++) {
			const uint32_t * show = line4 < 2 ? before : after;
			unsigned at = (line4 & 1) * 4;

			snprintf(line, sizeof(line), "%08X %08X %08X %08X", show[at], show[at + 1], show[at + 2],
			    show[at + 3]);
			dpuDrawText((uint8_t *) va1, vinfo.v_rowbytes, vinfo.v_width, 30, 400 + (int) line4 * 100, line, 4,
			    0x00ffffff);
			dpuDrawText((uint8_t *) vinfo.v_baseaddr, vinfo.v_rowbytes, vinfo.v_width, 30,
			    400 + (int) line4 * 100, line, 4, 0x00ffffff);
		}
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	/* (and then every register in the block that holds the address of the screen is pointed at the second one:
	 * the offsets the device tree gives are the ones the Linux driver uses, and this firmware was left by
	 * something else -- what it left the address in is found by looking for the address, not by knowing.) */
	{
		vm_offset_t mdp = ml_io_map(DPU_PHYS, DPU_LEN);
		uint32_t found = 0, where[4];
		char line[64];

		if (mdp != 0) {
			for (uint32_t at = 0; at < DPU_LEN; at += 4) {
				uint32_t v = dpuRd(mdp + at);

				if ((v & 0xfffff000) == (phys0 & 0xfffff000)) {
					if (found < 4) {
						where[found] = at;
					}
					found++;
					dpuWr(mdp + at, phys1 | (v & 0xfff));
				}
			}
			__asm__ volatile ("dsb sy" ::: "memory");
		}
		snprintf(line, sizeof(line), "%08X", found);
		dpuDrawText((uint8_t *) va1, vinfo.v_rowbytes, vinfo.v_width, 30, 800, line, 4, 0x00ffffff);
		dpuDrawText((uint8_t *) vinfo.v_baseaddr, vinfo.v_rowbytes, vinfo.v_width, 30, 800, line, 4, 0x00ffffff);
		snprintf(line, sizeof(line), "%05X %05X %05X %05X", found > 0 ? where[0] : 0, found > 1 ? where[1] : 0,
		    found > 2 ? where[2] : 0, found > 3 ? where[3] : 0);
		dpuDrawText((uint8_t *) va1, vinfo.v_rowbytes, vinfo.v_width, 30, 900, line, 4, 0x00ffffff);
		dpuDrawText((uint8_t *) vinfo.v_baseaddr, vinfo.v_rowbytes, vinfo.v_width, 30, 900, line, 4, 0x00ffffff);
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	ctl = ml_io_map(CTL0_PHYS, 0x1000);
	dpuWr(ctl + TEST_CTL_FLUSH, TEST_FLUSH_ALL);
	__asm__ volatile ("dsb sy" ::: "memory");
	dpuMark(240, 12);                       /* 5: the pipe has been told */

	/* (the frame goes on being drawn into the first screen: if the panel now shows the second one, it stays
	 * as it is, with the band on it, and that is what says the flip happened) */
	for (spin = 0; spin < 60000; spin++) {
		IOSleep(1);
	}
	IOLog("MiniIOMobileFramebuffer: the flip was written: %#x -> %#x, flush %#x; watch the panel\n", phys0, phys1,
	    TEST_FLUSH_ALL);
	for (;;) {
		IOSleep(1000);
	}
}

/*
 * Boot-arg te_probe=1: is the panel's TE pin moving, and how often.
 *
 * star's panel node gives platform-te-gpio = tlmm 82, and a panel driven in command mode pulls that
 * pin once a frame -- it is the only reference to where the panel is in its own frame that this
 * driver could have, and what a frame that must not tear has to be written against. Nothing here is
 * configured: only the pin's level register is read, so whatever the firmware muxed the pin to is
 * left as it is (a pin whose input buffer is off in that function reads as a constant; boot-arg
 * te_mux=1 then makes it a plain GPIO input first, which does take it away from the DSI controller).
 */
#define TE_TLMM_PHYS    0x0f100000UL
#define TE_TLMM_LEN     0x00100000UL
#define TE_PIN          82

static TUNABLE(uint32_t, gTeMux, "te_mux", 0);

/* ---- two screens, and the panel pointed at the whole one -------------------------------------------- */

/*
 * Boot-arg fb_flip=1: the panel is shown one screen while the next frame is drawn in the other, and
 * pointed at that one once the frame is whole.
 *
 * The tear this ends: a swap is a copy -- this driver has no flip -- so every frame was scaled by the
 * CPU straight into the memory the panel was scanning at that moment, and the panel was therefore always
 * somewhere inside a frame that was half the one before and half the next. With fb_threads the halves
 * were written by different processors at different times as well, which is the bands that slide over
 * each other.
 *
 * Nothing is torn out of memory that is not being written. Two screens: the one the panel is on, and the
 * one being drawn in. When the frame is whole, the address the pipes fetch from (their + 0x14) is written,
 * and the control that owns them is told to latch -- that write and that flush are the whole of the flip
 * (fb_blink found them: the address, then the flush). The next frame is then drawn in the screen the panel
 * has just left, which nothing is reading.
 *
 * (The panel is pointed at the other screen with the frame already in it, so a flip that lands in the
 * middle of a scan can only ever join two whole frames, and never two halves of one.)
 */
static void
flipPrepare(void)
{
	static const uint32_t pageAt[8] = { 0x05000, 0x07000, 0x09000, 0x0b000,
		0x25000, 0x27000, 0x29000, 0x2b000 };	/* (star.dtb: sde-sspp-off) */
	vm_offset_t gcc, dispcc, at;
	uint32_t on;

	if (fFlipScreen[1] == NULL) {
		return;
	}
	/* the two clocks the block's registers need: with either of them off a read of the block faults */
	gcc = ml_io_map(GCC_PHYS, 0x80000);
	if (gcc != 0) {
		on = dpuRd(gcc + GCC_DISP_AHB_CBCR);
		if (!(on & DPU_CBCR_ENABLE)) {
			dpuWr(gcc + GCC_DISP_AHB_CBCR, on | DPU_CBCR_ENABLE);
			IOSleep(10);
		}
	}
	dispcc = ml_io_map(DISPCC_PHYS, 0x8000);
	if (dispcc != 0) {
		on = dpuRd(dispcc + DISPCC_MDSS_AHB_CBCR);
		if (on & DPU_CBCR_OFF) {
			dpuWr(dispcc + DISPCC_MDSS_AHB_CBCR, on | DPU_CBCR_ENABLE);
			IOSleep(10);
		}
	}
	for (unsigned c = 0; c < 3; c++) {
		fFlipCtl[c] = ml_io_map(CTL0_PHYS + c * 0x1000, 0x1000);
	}
	/* the interfaces' frame counters: each steps at the top of its own frame, which is where the flip
	 * goes (see flipWaitBoundary()). read here with the same bus clocks on as the pipes below, and each
	 * one's line counter read twice 2 ms apart to see which of them is actually scanning. */
	fFlipIntf = ml_io_map(INTF_PHYS, 0x1000);
	fFlipIntf2 = gFlipIntfB != 0x36000 ? ml_io_map(DPU_PHYS + gFlipIntfB, 0x1000) : 0;
	{
		uint32_t a1 = fFlipIntf != 0 ? dpuRd(fFlipIntf + INTF_LINE_COUNT) : 0;
		uint32_t b1 = fFlipIntf2 != 0 ? dpuRd(fFlipIntf2 + INTF_LINE_COUNT) : 0;

		IOSleep(2);
		fFlipIntfOn[0] = fFlipIntf != 0 && dpuRd(fFlipIntf + INTF_LINE_COUNT) != a1;
		fFlipIntfOn[1] = fFlipIntf2 != 0 && dpuRd(fFlipIntf2 + INTF_LINE_COUNT) != b1;
		IOLog("flip: intf %#x scanning %u (frame %u, line %u of %u), intf %#x scanning %u (frame %u, line %u)\n",
		    (uint32_t) INTF_PHYS, fFlipIntfOn[0], fFlipIntf != 0 ? dpuRd(fFlipIntf + INTF_FRAME_COUNT) & 0xffff : 0,
		    a1 & 0xffff, fFlipIntf != 0 ? dpuRd(fFlipIntf + INTF_DISPLAY_V_END) & 0xffff : 0,
		    (uint32_t) (DPU_PHYS + gFlipIntfB), fFlipIntfOn[1],
		    fFlipIntf2 != 0 ? dpuRd(fFlipIntf2 + INTF_FRAME_COUNT) & 0xffff : 0, b1 & 0xffff);
	}
	/* which pipes fetch this screen is read out of them, not assumed: the one that is not fetching it is
	 * not using that register, and a write to it would mean nothing */
	for (unsigned p = 0; p < 8; p++) {
		at = ml_io_map(DPU_PHYS + pageAt[p], 0x1000);
		if (at == 0) {
			continue;
		}
		if (dpuRd(at + 0x014) == fFlipPhys[0] && fFlipPipes < kBlinkPipes) {
			fFlipAt[fFlipPipes++] = at;
		}
	}
	fFlipArmed = fFlipPipes != 0;
	IOLog("flip: boundary by frame step, line wrap or stillness\n");
	IOLog("flip: %u pipe(s) fetch %#x, the other screen is %#x: %s\n", fFlipPipes, fFlipPhys[0], fFlipPhys[1],
	    fFlipArmed ? "flipping" : "no pipe fetches the screen, so no flip");
}

/*
 * The panel's own frame boundary, waited for rather than guessed at. The only moment a flip can be made
 * without the scan catching it half done -- and a band of the new frame over the old one, or the two
 * halves of the panel disagreeing, is exactly that.
 *
 * Two ways to see the top of a frame, because the one that is cheapest to read is not always running:
 *   - the frame counter steps: that is a frame the timing engine finished handing over. It only steps
 *     when something new was given to it, so on a screen that has gone quiet (the telephone is not being
 *     touched, the compositor has nothing to send) it stands still for as long as the quiet lasts.
 *   - the line counter wraps: it runs 0..height while the scan is going, and the jump from the bottom
 *     back to the top is the blanking -- the same instant, seen from the side that never stops.
 *
 * The line counter is therefore the real clock and the frame counter is only the cheaper hint. Waiting
 * for the frame counter alone is what tore the first frame after a touch: the counter had not stepped
 * for a second, the wait timed out, and the flip went in wherever the scan happened to be.
 *
 * The scan lines count up through the panel's active height and then through a vertical-blanking slice
 * before wrapping. A flip written anywhere in the active slice is torn (the panel is mid-read); one
 * written in the blanking slice is not seen. So instead of guessing the top from a wrap, the counter's
 * own range is watched and the address is written only in that top slice -- which is found without ever
 * knowing the panel's timing. A line counter that does not move at all means the scan is not running,
 * and a flip with no scan to tear it is safe -- that is a success, not a timeout.
 *
 * This wait always ends in a flip. It used to give up and return false so the frame was skipped, but a
 * skipped frame leaves the panel on the picture before it, and the frame after that is drawn into the
 * same screen (the panel never moved) and skipped as well -- the screen stops changing for good, which
 * is how a burst of touches could freeze the picture. One tear is the price of never doing that.
 */
static bool
flipWaitBoundary(void)
{
	uint64_t began, limit = 0, now;
	vm_offset_t which;
	uint32_t lineFrom, line, lastLine;
	bool sane;

	if (gFlipIntfSel == 1 && fFlipIntf2 != 0) {
		which = fFlipIntf2;
	} else if (fFlipIntf != 0) {
		which = fFlipIntf;
	} else if (fFlipIntf2 != 0) {
		which = fFlipIntf2;
	} else {
		return false;
	}
	lineFrom = dpuRd(which + INTF_LINE_COUNT) & 0xffff;
	lastLine = lineFrom;
	began = mach_absolute_time();
	nanoseconds_to_absolutetime((uint64_t) gFlipEdgeMs * NSEC_PER_MSEC, &limit);
	limit += began;
	for (;;) {
		line = dpuRd(which + INTF_LINE_COUNT) & 0xffff;
		now = mach_absolute_time();
		/* (a register read of a block whose bus has gone quiet comes back as all ones, and 65535 is
		 * not a scan line. One such read was enough to put the high-water mark -- and with it the
		 * blanking window -- somewhere the counter never reaches, after which every frame would be
		 * skipped and the panel would sit on one picture while the system kept drawing. A line this
		 * far past the panel is a reading error, not a scan line, and changes nothing) */
		sane = line <= (fFlipPanelH != 0 ? fFlipPanelH * 2 : 8192);
		if (!sane) {
			fFlipLineBad++;
		} else {
			/* (track the counter's top so the blanking window -- the lines above the active height --
			 * can be located by the counter itself, with no timing numbers from the panel) */
			if (line > fFlipLineMax) {
				fFlipLineMax = line;
			}
			/* (the active region is fFlipPanelH lines; from there up to fFlipLineMax the scan is in
			 * vertical blanking, where a flip is invisible. Flip at the middle of that window: half
			 * a blanking's margin before the next active line, and half after the one just drawn) */
			if (fFlipLineMax > fFlipPanelH + 16) {
				/* (blanking window established by the counter's own range: flip only inside it) */
				uint32_t blank = fFlipLineMax - fFlipPanelH;
				uint32_t blankStart = fFlipPanelH + blank / 2;

				if (line >= blankStart) {
					fFlipEdgeKind = 2;
					break;
				}
			} else if (line + 8 < lastLine) {
				/* (no blanking in the counter's range yet -- the wrap itself is the frame boundary,
				 * so flip there; this is the fall-back when the counter spans only the active lines) */
				fFlipEdgeKind = 2;
				break;
			} else {
				lastLine = line;
			}
		}
		if (now > limit) {
			if (sane && line == lineFrom) {
				/* (nothing moved in all this time: no scan, nothing to tear -- flip now) */
				fFlipEdgeKind = 3;
				break;
			}
			/* (one whole window and the blanking was never reached: hand the frame over anyway.
			 * Skipping it would leave the panel on the frame before, and the next frame would be
			 * drawn into the same screen and skipped again -- a picture that stops changing, which
			 * is far worse than the tear this wait exists to avoid. The high-water mark is re-seeded
			 * from this reading so the window is found again on the next frame) */
			fFlipLineMax = sane ? line : 0;
			fFlipEdgeKind = 5;
			fFlipEdgeMiss++;
			break;
		}
	}
	fFlipEdgeUs = (uint32_t) micros(now - began);
	fFlipLineAt = dpuRd(which + INTF_LINE_COUNT) & 0xffff;
	fFlipLineB = fFlipIntf2 != 0 ? dpuRd(fFlipIntf2 + INTF_LINE_COUNT) & 0xffff : 0;
	if (fFlipEdgeKind != 5) {
		fFlipEdgeMiss = 0;
	}
	return true;
}

/*
 * The frame is in `back`; point the panel at it. `began` is when the frame's beat came, `drawn` when the
 * last pixel of it was written; fb_flip_us puts the write that far into the refresh (0: as soon as the
 * frame is whole).
 */
static void
flipPresent(uint32_t back, uint64_t began, uint64_t drawn)
{
	uint64_t at = 0;
	uint64_t gap = 0;

	if (gFlipEdge != 0) {
		/* (if the panel has gone a long time without being handed a frame, the wait is what is
		 * stuck -- hand this one over at once, so a wait can never be the thing that freezes the
		 * picture: half a second late is already a screen that stopped changing) */
		if (fFlipLastAt != 0) {
			absolutetime_to_nanoseconds(mach_absolute_time() - fFlipLastAt, &gap);
		}
		if (gap > 500 * NSEC_PER_MSEC) {
			fFlipStall++;
			fFlipEdgeKind = 6;
		} else {
			flipWaitBoundary();
		}
		if (gFlipLead != 0) {
			uint64_t lead = 0;

			nanoseconds_to_absolutetime((uint64_t) gFlipLead * NSEC_PER_USEC, &lead);
			clock_delay_until(mach_absolute_time() + lead);
		}
	} else if (gFlipUs != 0) {
		nanoseconds_to_absolutetime((uint64_t) gFlipUs * NSEC_PER_USEC, &at);
		if (began + at > drawn) {
			clock_delay_until(began + at);  /* (a deadline gone by returns at once) */
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	fFlipWaitUs = (uint32_t) (micros(mach_absolute_time() - began) & 0xffffffff);
	for (unsigned i = 0; i < fFlipPipes; i++) {
		dpuWr(fFlipAt[i] + 0x014, fFlipPhys[back]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	/* (the control latches what its pipes were told; a bit for a pipe that is not there clears nothing) */
	for (unsigned c = 0; c < 3; c++) {
		if (fFlipCtl[c] != 0 && (gFlipCtl & (1u << c))) {
			dpuWr(fFlipCtl[c] + TEST_CTL_FLUSH, gFlipFlush);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	fFlipFront = back;
	fFlipCount++;
	fFlipLastAt = mach_absolute_time();
	{
		uint64_t late = 0;

		absolutetime_to_nanoseconds(fFlipLastAt - drawn, &late);
		if (late > 200 * NSEC_PER_MSEC) {
			/* (a flip this late is a compositor that is being held up: say so in the log, where it
			 * can be read after the picture has stopped, not just on the picture) */
			fFlipSlow++;
			if (fFlipSlow <= 4) {
				IOLog("MiniIOMobileFramebuffer: frame %u was %llu ms late being handed to the panel (kind %u)\n",
				    fFlipCount, late / NSEC_PER_MSEC, fFlipEdgeKind);
			}
		}
	}
}

/*
 * fb_flip_beat=N: a column of squares that grows in the screen the panel is on, one more square every N
 * milliseconds, and starts over when it reaches sixteen.
 *
 * It writes where the scan is reading, with no lock and no flip, so it shows up at once and nothing can
 * be waiting behind it. That makes it the one thing that can still be seen when the picture has stopped
 * changing: squares that keep appearing mean the kernel is running and something in the compositing or in
 * a wait has stopped; squares that stop too mean the kernel is gone (a panic leaves the panel on its last
 * frame, with nothing drawn over it) or the display block has stopped scanning.
 */
static void
beaconThread(void * self __unused, wait_result_t result __unused)
{
	while (gFlipBeat != 0) {
		uint8_t * front;
		uint32_t tick;

		IOSleep(gFlipBeat);
		if (fFlipScreen[fFlipFront] == NULL || fFlipRowBytes == 0 || fFlipPanelH < 512) {
			continue;
		}
		front = fFlipScreen[fFlipFront];
		tick = ++fBeatTick;
		for (uint32_t i = 0; i < (tick % 16) + 1; i++) {
			uint32_t y = 40 + i * 40;
			uint32_t * to;

			if (y + 32 >= fFlipPanelH) {
				break;
			}
			to = (uint32_t *) (front + (size_t) y * fFlipRowBytes);
			for (uint32_t dy = 0; dy < 32; dy++) {
				uint32_t * row = to + (size_t) dy * (fFlipRowBytes / 4);

				for (uint32_t x = 40; x < 72 && x < fFlipPanelW; x++) {
					row[x] = ((tick + i) & 1) ? 0x0000ffff : 0x00ff00ff;
				}
			}
		}
	}
}

/*
 * Boot-arg fb_blink=1: one write at a time, and a step number that says how far it got.
 *
 * Late (twenty seconds: the system is drawing by then) and only once both clock controllers say the block's
 * register bus is running. The eight pipes' pages are read, the pipes that hold the boot screen's address at
 * +0x14 are kept, a second screen is filled with magenta (so a panel that starts showing it is not a
 * question of opinion), and then: the address, then a flush on each control in turn. Five seconds between
 * steps, and the loop starts over, so the panel blinks for as long as the phone lives.
 */
static void
blinkThread(void * self __unused, wait_result_t result __unused)
{
	static const uint32_t pageAt[8] = { 0x05000, 0x07000, 0x09000, 0x0b000,
		0x25000, 0x27000, 0x29000, 0x2b000 };	/* (star.dtb: sde-sspp-off) */
	vm_offset_t ctl[3];
	uint32_t on, clock;
	vm_offset_t gcc, dispcc, at;

	if (gBlink == 0 || fBlinkScreen == NULL) {
		return;
	}
	IOSleep(20000);
	gcc = ml_io_map(GCC_PHYS, 0x80000);
	on = dpuRd(gcc + GCC_DISP_AHB_CBCR);
	if (!(on & DPU_CBCR_ENABLE)) {
		dpuWr(gcc + GCC_DISP_AHB_CBCR, on | DPU_CBCR_ENABLE);
		IOSleep(10);
		on = dpuRd(gcc + GCC_DISP_AHB_CBCR);
	}
	if (!(on & DPU_CBCR_ENABLE)) {
		IOLog("blink: the display block's bus clock is off (gcc %#x)\n", on);
		return;
	}
	dispcc = ml_io_map(DISPCC_PHYS, 0x8000);
	clock = dpuRd(dispcc + DISPCC_MDSS_AHB_CBCR);
	if (clock & DPU_CBCR_OFF) {
		dpuWr(dispcc + DISPCC_MDSS_AHB_CBCR, clock | DPU_CBCR_ENABLE);
		IOSleep(10);
		clock = dpuRd(dispcc + DISPCC_MDSS_AHB_CBCR);
	}
	if (clock & DPU_CBCR_OFF) {
		IOLog("blink: the block's own bus clock is off (dispcc %#x)\n", clock);
		return;
	}
	for (unsigned c = 0; c < 3; c++) {
		ctl[c] = ml_io_map(CTL0_PHYS + c * 0x1000, 0x1000);
	}
	for (unsigned p = 0; p < 8; p++) {
		at = ml_io_map(DPU_PHYS + pageAt[p], 0x1000);
		if (at == 0) {
			continue;
		}
		if (dpuRd(at + 0x014) == fBlinkPhys[0] && fBlinkPipesFound < kBlinkPipes) {
			fBlinkAt[fBlinkPipesFound++] = at;
			IOLog("blink: pipe %u (%#x) fetches the screen at +0x14\n", p, pageAt[p]);
		}
	}
	fBlinkStep = 1;
	IOLog("blink: %u pipe(s) kept, second screen at %#x\n", fBlinkPipesFound, fBlinkPhys[1]);
	if (fBlinkPipesFound == 0) {
		return;
	}
	/* (the second screen: magenta, and written before the panel can see it) */
	{
		uint32_t * pixels = (uint32_t *)(void *) fBlinkScreen;
		uint32_t count = fBlinkBytes / 4;

		for (uint32_t i = 0; i < count; i++) {
			pixels[i] = 0x00ff00ff;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	IOSleep(5000);
	for (;;) {
		/* the address only, nothing else */
		fBlinkStep = 2;
		__asm__ volatile ("dsb sy" ::: "memory");
		for (unsigned i = 0; i < fBlinkPipesFound; i++) {
			dpuWr(fBlinkAt[i] + 0x014, fBlinkPhys[1]);
		}
		IOSleep(5000);

		fBlinkStep = 3;
		dpuWr(ctl[0] + TEST_CTL_FLUSH, (1u << 0) | (1u << 1));
		IOSleep(5000);

		fBlinkStep = 4;
		dpuWr(ctl[1] + TEST_CTL_FLUSH, (1u << 0) | (1u << 1));
		IOSleep(5000);

		fBlinkStep = 5;
		dpuWr(ctl[2] + TEST_CTL_FLUSH, (1u << 0) | (1u << 1));
		IOSleep(5000);

		fBlinkStep = 6;
		dpuWr(ctl[0] + TEST_CTL_FLUSH, (1u << 0) | (1u << 1) | (1u << 17));
		IOSleep(5000);

		/* the boot screen's address back, so the phone is left as it was found */
		fBlinkStep = 7;
		for (unsigned i = 0; i < fBlinkPipesFound; i++) {
			dpuWr(fBlinkAt[i] + 0x014, fBlinkPhys[0]);
		}
		for (unsigned c = 0; c < 3; c++) {
			dpuWr(ctl[c] + TEST_CTL_FLUSH, (1u << 0) | (1u << 1));
		}
		IOSleep(5000);
	}
}

static void
teProbeThread(void * self __unused, wait_result_t result __unused)
{
	vm_offset_t tlmm, io;
	uint64_t began, now, edgeAt, since, elapsed, us;
	uint32_t last, edges, reads;
	unsigned round = 0;

	IOSleep(25000);                         /* (the system is drawing by then) */
	tlmm = ml_io_map(TE_TLMM_PHYS, TE_TLMM_LEN);
	if (tlmm == 0) {
		IOLog("te: no memory for the pin\n");
		return;
	}
	if (gTeMux != 0) {
		/* function 0 (plain GPIO), no pull, 2 mA: input only, the level is not driven */
		dpuWr(tlmm + 0x1000UL * TE_PIN, 0);
		IOLog("te: pin %u made a GPIO input\n", TE_PIN);
	}
	io = tlmm + 0x1000UL * TE_PIN + 4;
	for (;;) {
		round++;
		last = dpuRd(io) & 1;
		edges = 0;
		reads = 0;
		edgeAt = began = mach_absolute_time();
		do {
			uint32_t level = dpuRd(io) & 1;

			reads++;
			if (level != last) {
				edges++;
				if (edges <= 6) {
					now = mach_absolute_time();
					absolutetime_to_nanoseconds(now - edgeAt, &since);
					us = since / 1000;
					IOLog("te: %u: edge %u at level %u, %llu us after the one before\n", round, edges,
					    level, us);
					edgeAt = now;
				}
				last = level;
			}
			if ((reads & 0x3ffff) == 0) {
				absolutetime_to_nanoseconds(mach_absolute_time() - began, &since);
				if (since >= 500 * NSEC_PER_MSEC) {
					break;
				}
			}
		} while (true);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - began, &elapsed);
		IOLog("te: %u: %u edges (level now %u) in %llu us, %u reads%s\n", round, edges, last,
		    elapsed / 1000, reads, edges == 0 ? ": the pin does not move (try te_mux=1)" : "");
		IOSleep(4000);
	}
}
