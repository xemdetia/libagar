/*
 * Copyright (c) 2009-2012 Hypertriton, Inc. <http://hypertriton.com/>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Driver for OpenGL graphics via X Windows System. This is a multiple display
 * driver; one context is created for each Agar window.
 */

#include <config/have_xkb.h>
#include <config/have_xinerama.h>
#include <config/have_kqueue.h>
#include <config/have_timerfd.h>

#include <core/core.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/cursorfont.h>
#ifdef HAVE_XKB
#include <X11/XKBlib.h>
#endif
#ifdef HAVE_XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

#include "gui.h"
#include "drv.h"
#include "text.h"
#include "window.h"
#include "gui_math.h"
#include "cursors.h"
#include "opengl.h"

#if defined(HAVE_KQUEUE)
# include <sys/types.h>
# include <sys/event.h>
# include <sys/time.h>
# include <errno.h>
#elif defined(HAVE_TIMERFD)
# include <sys/select.h>
# include <errno.h>
#endif

/* #define DEBUG_XSYNC */

static int      nDrivers = 0;		/* Drivers open */
static Display *agDisplay = NULL;	/* X display (shared) */
static int      agScreen = 0;		/* Default screen (shared) */
static Uint     rNom = 20;		/* Nominal refresh rate (ms) */
static int      rCur = 0;		/* Effective refresh rate (ms) */
static AG_Mutex agDisplayLock;		/* Lock on agDisplay */
static int      agExitGLX = 0;		/* Exit event loop */

static char           xkbBuf[64];	/* For Unicode key translation */
static XComposeStatus xkbCompStatus;	/* For Unicode key translation */

/* Driver instance data */
typedef struct ag_driver_glx {
	struct ag_driver_mw _inherit;
	Window           w;		/* X window */
	GLXContext       glxCtx;	/* GLX context */

	AG_GL_Context    gl;		/* Common OpenGL context data */
	AG_Mutex         lock;		/* Protect Xlib calls */
	int              wmHintsSet;	/* WM hints have been set */
} AG_DriverGLX;

static int modMasksInited = 0;		/* For modifier key translation */
struct {
	Uint Lmeta, Rmeta;
	Uint Lalt, Ralt;
	Uint num, mode;
} modMasks;
struct ag_glx_key_mapping {		/* Keymap translation table entry */
	int kcode;			/* Scancode */
	int kclass;			/* X keysym class (e.g., 0xff) */
	AG_KeySym key;			/* Corresponding Agar keysym */
};
#include "drv_glx_keymaps.h"

typedef struct ag_cursor_glx {
	XColor black;
	XColor white;
	Cursor xc;
	int visible;
} AG_CursorGLX;

AG_DriverMwClass agDriverGLX;
#if 0
#define AGDRIVER_IS_GLX(drv) \
	AG_OfClass((drv),"AG_Driver:AG_DriverMw:AG_DriverGLX")
#endif
#define AGDRIVER_IS_GLX(drv) \
	(AGDRIVER_CLASS(drv) == (AG_DriverClass *)&agDriverGLX)

/* X Atoms */
static Atom wmProtocols, wmTakeFocus, wmDeleteWindow,
    wmMotifWmHints, wmKwmWinDecoration, wmWinHints,
    wmNetWmState, wmNetWmStateModal, wmNetWmStateSkipTaskbar,
    wmNetWmStateAbove, wmNetWmStateBelow, wmNetWmWindowType,
    wmNetWmWindowOpacity;

static void GLX_PostResizeCallback(AG_Window *, AG_SizeAlloc *);
static void GLX_PostMoveCallback(AG_Window *, AG_SizeAlloc *);
static int  GLX_RaiseWindow(AG_Window *);
static int  GLX_SetInputFocus(AG_Window *);
static void GLX_SetTransientFor(AG_Window *, AG_Window *);
#if 0
static void GLX_FreeWidgetResources(AG_Widget *);
#endif

static void
Init(void *obj)
{
	AG_DriverGLX *glx = obj;
	AG_DriverMw *dmw = obj;

	dmw->flags |= AG_DRIVER_MW_ANYPOS_AVAIL;
	glx->w = 0;
	glx->wmHintsSet = 0;
	AG_MutexInitRecursive(&glx->lock);
}

static void
Destroy(void *obj)
{
	AG_DriverGLX *glx = obj;

	AG_MutexDestroy(&glx->lock);
}

/*
 * Driver initialization
 */

static int
InitGlobals(void)
{
	int err, ev;

	if (nDrivers > 0)
		return (0);

#ifdef AG_THREADS
	XInitThreads();
#endif
	if ((agDisplay = XOpenDisplay(NULL)) == NULL) {
		AG_SetError("Cannot open X display");
		return (-1);
	}
	if (!glXQueryExtension(agDisplay, &err, &ev)) {
		AG_SetError("GLX extension is not available");
		XCloseDisplay(agDisplay);
		agDisplay = NULL;
		return (-1);
	}

	AG_MutexInitRecursive(&agDisplayLock);
	agScreen = DefaultScreen(agDisplay);
	InitKeymaps();
	memset(xkbBuf, '\0', sizeof(xkbBuf));
	memset(&xkbCompStatus, 0, sizeof(xkbCompStatus));
	wmProtocols = XInternAtom(agDisplay, "WM_PROTOCOLS", True);
	wmTakeFocus = XInternAtom(agDisplay, "WM_TAKE_FOCUS", True);
	wmDeleteWindow = XInternAtom(agDisplay, "WM_DELETE_WINDOW", True);
	wmMotifWmHints = XInternAtom(agDisplay, "_MOTIF_WM_HINTS", True);
	wmKwmWinDecoration = XInternAtom(agDisplay, "KWM_WIN_DECORATION", True);
	wmWinHints = XInternAtom(agDisplay, "_WIN_HINTS", True);
	wmNetWmState = XInternAtom(agDisplay, "_NET_WM_STATE", True);
	wmNetWmStateModal = XInternAtom(agDisplay, "_NET_WM_STATE_MODAL", True);
	wmNetWmStateSkipTaskbar = XInternAtom(agDisplay, "_NET_WM_STATE_SKIP_TASKBAR", True);
	wmNetWmStateAbove = XInternAtom(agDisplay, "_NET_WM_STATE_ABOVE", True);
	wmNetWmStateBelow = XInternAtom(agDisplay, "_NET_WM_STATE_BELOW", True);
	wmNetWmWindowType = XInternAtom(agDisplay, "_NET_WM_WINDOW_TYPE", True);
	wmNetWmWindowOpacity = XInternAtom(agDisplay, "_NET_WM_WINDOW_OPACITY", True);
	return (0);
}

/* on entry the agDisplayLock must be helt */
static void
DestroyGlobals(void)
{
	if (nDrivers > 0)
		return;

	if (agDisplay)
		XCloseDisplay(agDisplay);
	agDisplay = NULL;
	agScreen = 0;

	AG_MutexUnlock(&agDisplayLock);
	AG_MutexDestroy(&agDisplayLock);
}

#ifdef DEBUG_XSYNC
static int
HandleErrorX11(Display *disp, XErrorEvent *xev)
{
	char buf[128];

	XGetErrorText(disp, xev->error_code, buf, sizeof(buf));
	fprintf(stderr, "Caught X error: %s\n", buf);
	abort();
}
#endif /* DEBUG_XSYNC */

static int
GLX_Open(void *obj, const char *spec)
{
	AG_Driver *drv = obj;
	AG_DriverGLX *glx = obj;

	if (InitGlobals() == -1)
		return -1;

	AG_MutexLock(&agDisplayLock);
	nDrivers++;

#ifdef HAVE_KQUEUE
	/* Register a read event on the X file descriptor. */
	if (agKqueue != -1) {
		struct kevent kev;
		
		EV_SET(&kev, ConnectionNumber(agDisplay),
		    EVFILT_READ, EV_ADD|EV_ENABLE,
		    0, 0, (intptr_t) NULL);
		if (kevent(agKqueue, &kev, 1, NULL, 0, NULL) == -1) {
			AG_SetError("kqueue: %s", AG_Strerror(errno));
			goto fail;
		}
	}
#endif /* HAVE_KQUEUE */

	/* Register the core X mouse and keyboard */
	if ((drv->mouse = AG_MouseNew(glx, "X mouse")) == NULL ||
	    (drv->kbd = AG_KeyboardNew(glx, "X keyboard")) == NULL)
		goto fail;
	
	/* Driver manages rendering of window background. */
	drv->flags |= AG_DRIVER_WINDOW_BG;

#ifdef DEBUG_XSYNC
	/* Set up synchronous X events. */
	XSynchronize(agDisplay, True);
	XSetErrorHandler(HandleErrorX11);
#endif
	AG_MutexUnlock(&agDisplayLock);
	return (0);

fail:
	if (drv->kbd != NULL) {
		AG_ObjectDetach(drv->kbd);
		AG_ObjectDestroy(drv->kbd);
		drv->kbd = NULL;
	}
	if (drv->mouse != NULL) {
		AG_ObjectDetach(drv->mouse);
		AG_ObjectDestroy(drv->mouse);
		drv->mouse = NULL;
	}
	if (--nDrivers == 0) {
		DestroyGlobals();
		return (-1);
	}
	AG_MutexUnlock(&agDisplayLock);
	return (-1);
}

static void
GLX_Close(void *obj)
{
	AG_Driver *drv = obj;

#ifdef AG_DEBUG
	if (nDrivers == 0) { AG_FatalError("Driver close without open"); }
#endif
	AG_MutexLock(&agDisplayLock);
	if (drv->kbd) {
		AG_ObjectDetach(drv->kbd);
		AG_ObjectDestroy(drv->kbd);
		drv->kbd = NULL;
	}
	if (drv->mouse) {
		AG_ObjectDetach(drv->mouse);
		AG_ObjectDestroy(drv->mouse);
		drv->mouse = NULL;
	}
	if (--nDrivers == 0) {
		DestroyGlobals();
		return;
	}
	AG_MutexUnlock(&agDisplayLock);
}

static int
GLX_GetDisplaySize(Uint *w, Uint *h)
{
	AG_MutexLock(&agDisplayLock);
#ifdef HAVE_XINERAMA
	{
		int event, error, nScreens;
		XineramaScreenInfo *xs;

		if (XineramaQueryExtension(agDisplay, &event, &error) &&
		    (xs = XineramaQueryScreens(agDisplay, &nScreens)) != NULL) {
			*w = (Uint)xs[0].width;
			*h = (Uint)xs[0].height;
			XFree(xs);
			goto out;
		}
	}
#endif /* HAVE_XINERAMA */
	*w = (Uint)DisplayWidth(agDisplay, agScreen);
	*h = (Uint)DisplayHeight(agDisplay, agScreen);
out:
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

/*
 * Get Agar keycode corresponding to an X KeyCode.
 * agDisplayLock must be held.
 */
static int
LookupKeyCode(KeyCode keycode, AG_KeySym *rv)
{
	KeySym ks;

#ifdef HAVE_XKB
	if ((ks = XkbKeycodeToKeysym(agDisplay, keycode, 0, 0)) == 0) {
		*rv = AG_KEY_NONE;
		return (0);
	}
#else
	if ((ks = XKeycodeToKeysym(agDisplay, keycode, 0)) == 0) {
		*rv = AG_KEY_NONE;
		return (0);
	}
#endif

	switch (ks>>8) {
#ifdef XK_MISCELLANY
	case 0xff:
		*rv = agKeymapMisc[ks&0xff];
		return (1);
#endif
#ifdef XK_XKB_KEYS
	case 0xfe:
		*rv = agKeymapXKB[ks&0xff];
		return (1);
#endif
	default:
		*rv = (AG_KeySym)(ks&0xff);
		return (1);
	}
	return (0);
}

/* Return the Agar window corresponding to a X Window ID */
static __inline__ AG_Window *
LookupWindowByID(Window xw)
{
	AG_Window *win;
	AG_DriverGLX *glx;

	/* XXX TODO portable to optimize based on numerical XIDs? */
	AG_LockVFS(&agDrivers);
	AGOBJECT_FOREACH_CHILD(glx, &agDrivers, ag_driver_glx) {
		if (!AGDRIVER_IS_GLX(glx)) {
			continue;
		}
		if (glx->w == xw) {
			win = AGDRIVER_MW(glx)->win;
			if (WIDGET(win)->drv == NULL) {	/* Being detached */
				goto fail;
			}
			AG_UnlockVFS(&agDrivers);
			return (win);
		}
	}
fail:
	AG_UnlockVFS(&agDrivers);
	AG_SetError("X event from unknown window");
	return (NULL);
}

static void
InitModifierMasks(void)
{
	XModifierKeymap *xmk;
	int i, j;
	Uint n;

	if (modMasksInited) {
		return;
	}
	modMasksInited = 1;

	xmk = XGetModifierMapping(agDisplay);
	n = xmk->max_keypermod;
	for (i = 3; i < 8; i++) {
		for (j = 0; j < n; j++) {
			KeyCode kc = xmk->modifiermap[i*n + j];
#ifdef HAVE_XKB
			KeySym ks = XkbKeycodeToKeysym(agDisplay, kc, 0, 0);
#else
			KeySym ks = XKeycodeToKeysym(agDisplay, kc, 0);
#endif
			Uint mask = 1<<i;

#ifdef XK_MISCELLANY
			switch (ks) {
			case XK_Num_Lock:	modMasks.num = mask;	break;
			case XK_Alt_L:		modMasks.Lalt = mask;	break;
			case XK_Alt_R:		modMasks.Ralt = mask;	break;
			case XK_Meta_L:		modMasks.Lmeta = mask;	break;
			case XK_Meta_R:		modMasks.Rmeta = mask;	break;
			case XK_Mode_switch:	modMasks.mode = mask;	break;
			}
#endif /* XK_MISCELLANY */
		}
	}
	XFreeModifiermap(xmk);
}

/* Refresh the internal keyboard state. */
static void
UpdateKeyboard(AG_Keyboard *kbd, char kv[32])
{
	int i, j;
	Uint ms = 0;
	AG_KeySym key;
	Window dummy;
	int x, y;
	Uint mask;
	
	AG_MutexLock(&agDisplayLock);

	/* Get the keyboard modifier state. XXX */
	InitModifierMasks();
	if (XQueryPointer(agDisplay, DefaultRootWindow(agDisplay),
	    &dummy, &dummy, &x, &y, &x, &y, &mask)) {
		if (mask & LockMask) { ms |= AG_KEYMOD_CAPSLOCK; }
		if (mask & modMasks.mode) { ms |= AG_KEYMOD_MODE; }
		if (mask & modMasks.num) { ms |= AG_KEYMOD_NUMLOCK; }
	}
	memset(kbd->keyState, 0, kbd->keyCount);
	for (i = 0; i < 32; i++) {
		if (kv[i] == 0) {
			continue;
		}
		for (j = 0; j < 8; j++) {
			if ((kv[i] & (1<<j)) == 0) {
				continue;
			}
			if (!LookupKeyCode(i<<3|j, &key)) {
				continue;
			}
			kbd->keyState[key] = AG_KEY_PRESSED;

			switch (key) {
			case AG_KEY_LSHIFT: ms |= AG_KEYMOD_LSHIFT; break;
			case AG_KEY_RSHIFT: ms |= AG_KEYMOD_RSHIFT; break;
			case AG_KEY_LCTRL:  ms |= AG_KEYMOD_LCTRL;  break;
			case AG_KEY_RCTRL:  ms |= AG_KEYMOD_RCTRL;  break;
			case AG_KEY_LALT:   ms |= AG_KEYMOD_LALT;   break;
			case AG_KEY_RALT:   ms |= AG_KEYMOD_RALT;   break;
			case AG_KEY_LMETA:  ms |= AG_KEYMOD_LMETA;  break;
			case AG_KEY_RMETA:  ms |= AG_KEYMOD_RMETA;  break;
			default:
				break;
			}
		}
	}

	kbd->keyState[AG_KEY_CAPSLOCK] = (ms & AG_KEYMOD_CAPSLOCK) ?
	    AG_KEY_PRESSED : AG_KEY_RELEASED;
	kbd->keyState[AG_KEY_NUMLOCK] = (ms & AG_KEYMOD_NUMLOCK) ?
	    AG_KEY_PRESSED : AG_KEY_RELEASED;

	/* Set the final modifier state */
	kbd->modState = ms;
	
	AG_MutexUnlock(&agDisplayLock);
}

/* Refresh the internal keyboard state for all glx driver instances. */
static void
UpdateKeyboardAll(char kv[32])
{
	AG_Driver *drv;

	AG_LockVFS(&agDrivers);
	AGOBJECT_FOREACH_CHILD(drv, &agDrivers, ag_driver) {
		if (!AGDRIVER_IS_GLX(drv)) {
			continue;
		}
		UpdateKeyboard(drv->kbd, kv);
	}
	AG_UnlockVFS(&agDrivers);
}

static __inline__ int
GLX_PendingEvents(void *drvCaller)
{
	struct timeval tv;
	fd_set fdset;
	int fd, ret = 0;

	AG_MutexLock(&agDisplayLock);

	if (agDisplay == NULL)
		goto out;

	XFlush(agDisplay);

	if (XEventsQueued(agDisplay, QueuedAlready)) {
		ret = 1;
		goto out;
	}

	/* Poll the X connection fd */
	fd = ConnectionNumber(agDisplay);
	FD_ZERO(&fdset);
	FD_SET(fd, &fdset);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	if (select(fd+1, &fdset, NULL, NULL, &tv) == 1) {
		ret = XPending(agDisplay);
	}
out:
	AG_MutexUnlock(&agDisplayLock);
	return (ret);
}

/* The agDrivers VFS must be locked. */
static int
PostEventCallback(void *drvCaller)
{
	AG_Window *win;
	AG_Driver *drv;

	if (!TAILQ_EMPTY(&agWindowDetachQ))
		AG_FreeDetachedWindows();
	
	/*
	 * Exit when no more visible windows exist.
	 * XXX TODO make this behavior configurable
	 */
	if (TAILQ_EMPTY(&OBJECT(&agDrivers)->children)) {
		goto nowindows;
	}
	AGOBJECT_FOREACH_CHILD(drv, &agDrivers, ag_driver) {
		AG_FOREACH_WINDOW(win, drv) {
			if (win->visible)
				break;
		}
		if (win != NULL)
			break;
	}
	if (win == NULL)
		goto nowindows;

	if (agWindowToFocus != NULL) {
		AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(agWindowToFocus)->drv;

		if (glx != NULL && AGDRIVER_IS_GLX(glx)) {
			AG_MutexLock(&agDisplayLock);
			AG_MutexLock(&glx->lock);
			GLX_RaiseWindow(agWindowToFocus);
			GLX_SetInputFocus(agWindowToFocus);
			AG_MutexUnlock(&glx->lock);
			AG_MutexUnlock(&agDisplayLock);
		}
		agWindowToFocus = NULL;
	}
	return (1);
nowindows:
	AG_SetError("No more windows exist");
	agTerminating = 1;
	return (-1);
}

static int
GLX_GetNextEvent(void *drvCaller, AG_DriverEvent *dev)
{
	XEvent xev;
	int x, y;
	AG_KeySym ks;
	AG_Window *win;
	Uint32 ucs;

	AG_MutexLock(&agDisplayLock);
	XNextEvent(agDisplay, &xev);
	AG_MutexUnlock(&agDisplayLock);

	switch (xev.type) {
	case MotionNotify:
		if ((win = LookupWindowByID(xev.xmotion.window)) == NULL) {
			return (-1);
		}
		x = AGDRIVER_BOUNDED_WIDTH(win, xev.xmotion.x);
		y = AGDRIVER_BOUNDED_HEIGHT(win, xev.xmotion.y);
		AG_MouseMotionUpdate(WIDGET(win)->drv->mouse, x,y);
		
		dev->type = AG_DRIVER_MOUSE_MOTION;
		dev->win = win;
		dev->data.motion.x = x;
		dev->data.motion.y = y;
		break;
	case ButtonPress:
		if ((win = LookupWindowByID(xev.xbutton.window)) == NULL) {
			return (-1);
		}
		x = AGDRIVER_BOUNDED_WIDTH(win, xev.xbutton.x);
		y = AGDRIVER_BOUNDED_HEIGHT(win, xev.xbutton.y);
		AG_MouseButtonUpdate(WIDGET(win)->drv->mouse,
		    AG_BUTTON_PRESSED, xev.xbutton.button);

		dev->type = AG_DRIVER_MOUSE_BUTTON_DOWN;
		dev->win = win;
		dev->data.button.which = (AG_MouseButton)xev.xbutton.button;
		dev->data.button.x = xev.xbutton.x;
		dev->data.button.y = xev.xbutton.y;
		break;
	case ButtonRelease:
		if ((win = LookupWindowByID(xev.xbutton.window)) == NULL) {
			return (-1);
		}
		x = AGDRIVER_BOUNDED_WIDTH(win, xev.xbutton.x);
		y = AGDRIVER_BOUNDED_HEIGHT(win, xev.xbutton.y);
		AG_MouseButtonUpdate(WIDGET(win)->drv->mouse,
		    AG_BUTTON_RELEASED, xev.xbutton.button);

		dev->type = AG_DRIVER_MOUSE_BUTTON_UP;
		dev->win = win;
		dev->data.button.which = (AG_MouseButton)xev.xbutton.button;
		dev->data.button.x = xev.xbutton.x;
		dev->data.button.y = xev.xbutton.y;
		break;
	case KeyRelease:
		AG_MutexLock(&agDisplayLock);
		if (IsKeyRepeat(&xev)) {
			/* We implement key repeat internally */
			AG_MutexUnlock(&agDisplayLock);
			return (0);
		}
		AG_MutexUnlock(&agDisplayLock);
		/* FALLTHROUGH */
	case KeyPress:
		AG_MutexLock(&agDisplayLock);
		if (XLookupString(&xev.xkey, xkbBuf, sizeof(xkbBuf),
		    NULL, &xkbCompStatus) >= 1) {
			ucs = (Uint8)xkbBuf[0];	/* XXX */
		} else {
			ucs = 0;
		}
		if (!LookupKeyCode(xev.xkey.keycode, &ks)) {
			AG_SetError("Keyboard event: Unknown keycode: %d",
			    (int)xev.xkey.keycode);
			AG_MutexUnlock(&agDisplayLock);
			return (-1);
		}
		AG_MutexUnlock(&agDisplayLock);

		if ((win = LookupWindowByID(xev.xkey.window)) == NULL) {
			return (-1);
		}
		AG_KeyboardUpdate(WIDGET(win)->drv->kbd,
		    (xev.type == KeyPress) ? AG_KEY_PRESSED : AG_KEY_RELEASED,
		    ks, ucs);

		dev->type = (xev.type == KeyPress) ? AG_DRIVER_KEY_DOWN :
		                                     AG_DRIVER_KEY_UP;
		dev->win = win;
		dev->data.key.ks = ks;
		dev->data.key.ucs = ucs;
		break;
	case EnterNotify:
		if ((win = LookupWindowByID(xev.xcrossing.window)) == NULL) {
			return (-1);
		}
		dev->type = AG_DRIVER_MOUSE_ENTER;
		dev->win = win;
		break;
	case LeaveNotify:
		if ((win = LookupWindowByID(xev.xcrossing.window)) == NULL) {
			return (-1);
		}
		dev->type = AG_DRIVER_MOUSE_LEAVE;
		dev->win = win;
		break;
	case FocusIn:
		if ((win = LookupWindowByID(xev.xfocus.window)) == NULL) {
			return (-1);
		}
		agWindowFocused = win;
		AG_PostEvent(NULL, win, "window-gainfocus", NULL);
		dev->type = AG_DRIVER_FOCUS_IN;
		dev->win = win;
		break;
	case FocusOut:
		if ((win = LookupWindowByID(xev.xfocus.window)) == NULL) {
			return (-1);
		}
		if (agWindowFocused == win) {
			AG_PostEvent(NULL, win, "window-lostfocus", NULL);
			agWindowFocused = NULL;
		}
		dev->type = AG_DRIVER_FOCUS_OUT;
		dev->win = win;
		break;
	case KeymapNotify:					/* Internal */
		UpdateKeyboardAll(xev.xkeymap.key_vector);
		return (0);
	case MappingNotify:					/* Internal */
		AG_MutexLock(&agDisplayLock);
		XRefreshKeyboardMapping(&xev.xmapping);
		AG_MutexUnlock(&agDisplayLock);
		return (0);
	case ConfigureNotify:					/* Internal */
		if ((win = LookupWindowByID(xev.xconfigure.window))) {
			Window ignore;

			AG_MutexLock(&agDisplayLock);
			XTranslateCoordinates(agDisplay,
			    xev.xconfigure.window,
			    DefaultRootWindow(agDisplay),
			    0, 0,
			    &x, &y,
			    &ignore);
			AG_MutexUnlock(&agDisplayLock);

			dev->type = AG_DRIVER_VIDEORESIZE;
			dev->win = win;
			dev->data.videoresize.x = x;
			dev->data.videoresize.y = y;
			dev->data.videoresize.w = xev.xconfigure.width;
			dev->data.videoresize.h = xev.xconfigure.height;
			break;
		} else {
			return (-1);
		}
	case Expose:
		if ((win = LookupWindowByID(xev.xexpose.window)) == NULL) {
			return (-1);
		}
		dev->type = AG_DRIVER_EXPOSE;
		dev->win = win;
		break;
	case ClientMessage:
		if ((xev.xclient.format == 32) &&
		    (xev.xclient.data.l[0] == wmDeleteWindow) &&
		    (win = LookupWindowByID(xev.xclient.window))) {
			dev->type = AG_DRIVER_CLOSE;
			dev->win = win;
			break;
		}
		return (0);
	case MapNotify:
	case UnmapNotify:
	case DestroyNotify:
	case ReparentNotify:
		return (0);
	default:
		AG_SetError("Unknown X event %d\n", xev.type);
		return (-1);
	}
	return (1);
}

static int
GLX_ProcessEvent(void *drvCaller, AG_DriverEvent *dev)
{
	AG_Driver *drv;
	AG_SizeAlloc a;

	if (dev->win == NULL) {
		return (0);
	}
	AGOBJECT_FOREACH_CHILD(drv, &agDrivers, ag_driver) {
		if (WIDGET(dev->win)->drv == drv)
			break;
	}
	if (drv == NULL) {
		return (0);
	}
	drv = WIDGET(dev->win)->drv;

	switch (dev->type) {
	case AG_DRIVER_MOUSE_MOTION:
		AG_ProcessMouseMotion(dev->win,
		    dev->data.motion.x, dev->data.motion.y,
		    drv->mouse->xRel, drv->mouse->yRel,
		    drv->mouse->btnState);
		AG_MouseCursorUpdate(dev->win,
		     dev->data.motion.x, dev->data.motion.y);
		break;
	case AG_DRIVER_MOUSE_BUTTON_DOWN:
		AG_ProcessMouseButtonDown(dev->win,
		    dev->data.button.x, dev->data.button.y,
		    dev->data.button.which);
		break;
	case AG_DRIVER_MOUSE_BUTTON_UP:
		AG_ProcessMouseButtonUp(dev->win,
		    dev->data.button.x, dev->data.button.y,
		    dev->data.button.which);
		break;
	case AG_DRIVER_KEY_UP:
		AG_ProcessKey(drv->kbd, dev->win, AG_KEY_RELEASED,
		    dev->data.key.ks, dev->data.key.ucs);
		break;
	case AG_DRIVER_KEY_DOWN:
		AG_ProcessKey(drv->kbd, dev->win, AG_KEY_PRESSED,
		    dev->data.key.ks, dev->data.key.ucs);
		break;
	case AG_DRIVER_MOUSE_ENTER:
		AG_PostEvent(NULL, dev->win, "window-enter", NULL);
		break;
	case AG_DRIVER_MOUSE_LEAVE:
		AG_PostEvent(NULL, dev->win, "window-leave", NULL);
		break;
	case AG_DRIVER_FOCUS_IN:
		agWindowFocused = dev->win;
		AG_PostEvent(NULL, dev->win, "window-gainfocus", NULL);
		break;
	case AG_DRIVER_FOCUS_OUT:
		if (dev->win == agWindowFocused) {
			AG_PostEvent(NULL, dev->win, "window-lostfocus", NULL);
			agWindowFocused = NULL;
		}
		break;
	case AG_DRIVER_VIDEORESIZE:
		a.x = dev->data.videoresize.x;
		a.y = dev->data.videoresize.y;
		a.w = dev->data.videoresize.w;
		a.h = dev->data.videoresize.h;
		if (a.x != WIDGET(dev->win)->x || a.y != WIDGET(dev->win)->y) {
			GLX_PostMoveCallback(dev->win, &a);
		}
		if (a.w != WIDTH(dev->win) || a.h != HEIGHT(dev->win)) {
			GLX_PostResizeCallback(dev->win, &a);
		}
		break;
	case AG_DRIVER_CLOSE:
		AG_PostEvent(NULL, dev->win, "window-close", NULL);
		break;
	case AG_DRIVER_EXPOSE:
		dev->win->dirty = 1;
		break;
	default:
		break;
	}
	return PostEventCallback(drvCaller);
}

/* Render any window marked dirty. */ 
static void
RenderDirtyWindows(void)
{
	AG_Driver *drv;
	AG_DriverGLX *glx;
	AG_Window *win;

	AG_LockVFS(&agDrivers);
	AGOBJECT_FOREACH_CHILD(drv, &agDrivers, ag_driver) {
		if (!AGDRIVER_IS_GLX(drv)) {
			continue;
		}
		glx = (AG_DriverGLX *)drv;
		AG_MutexLock(&glx->lock);
		win = AGDRIVER_MW(drv)->win;
		if (win->visible && win->dirty) {
			AG_BeginRendering(drv);
			AG_ObjectLock(win);
			AG_WindowDraw(win);
			AG_ObjectUnlock(win);
			AG_EndRendering(drv);
		}
		AG_MutexUnlock(&glx->lock);
	}
	AG_UnlockVFS(&agDrivers);
}

/* Generic application event loop (using delay loops). */
static void
GLX_GenericEventLoop(void *obj)
{
	AG_DriverEvent dev;
	Uint32 t1, t2;

	t1 = AG_GetTicks();
	for (;;) {
		t2 = AG_GetTicks();
		if (agExitGLX) {
			break;
		} else if (t2 - t1 >= rNom) {
			RenderDirtyWindows();
			t1 = AG_GetTicks();
			rCur = rNom - (t1-t2);
			if (rCur < 1) { rCur = 1; }
		} else if (GLX_PendingEvents(NULL) != 0) {
			AG_LockVFS(&agDrivers);
			if (GLX_GetNextEvent(NULL, &dev) == 1 &&
			    GLX_ProcessEvent(NULL, &dev) == -1) {
				AG_UnlockVFS(&agDrivers);
				return;
			}
			AG_UnlockVFS(&agDrivers);
		} else {
			AG_ProcessTimeouts(t2);
			AG_Delay(1);
		}
	}
}

#if defined(HAVE_KQUEUE)
/*
 * Generic application event loop (using BSD kqueue(2) interface).
 */
static void
GLX_GenericEventLoop_KQUEUE(void *obj)
{
	struct kevent kev;
	AG_DriverEvent dev;
	int nev, rv = 0;
	AG_Object *ob;
	AG_Timer *to;
	Uint32 rvt;

	if (agKqueue == -1) {
		GLX_GenericEventLoop(obj);		/* Fallback */
		return;
	}
	memset(&kev, 0, sizeof(struct kevent));		/* For valgrind */
	for (;;) {
		if ((nev = kevent(agKqueue, NULL, 0, &kev, 1, NULL)) < 0) {
			AG_SetError("kevent(): %s", AG_Strerror(errno));
			rv = -1;
			goto out;
		} else if (nev < 1) {
			continue;
		}
		if (kev.flags & EV_ERROR) {
			AG_SetError("kevent: %s", AG_Strerror(kev.data));
			rv = -1;
			goto out;
		}
		switch (kev.filter) {
		case EVFILT_READ:
			AG_LockVFS(&agDrivers);
			while (GLX_PendingEvents(NULL) != 0) {
				if (GLX_GetNextEvent(NULL, &dev) != 1) {
					break;
				}
				if (GLX_ProcessEvent(NULL, &dev) == -1) {
					AG_UnlockVFS(&agDrivers);
					goto out;
				}
			}
			AG_UnlockVFS(&agDrivers);
			break;
		case EVFILT_TIMER:
			to = (AG_Timer *)kev.udata;
			ob = to->obj;
			AG_LockTimers(ob);
			AG_LockVFS(&agDrivers);
			rvt = to->fn(to, &to->fnEvent);
			AG_UnlockVFS(&agDrivers);
			if (rvt > 0) {				/* Restart */
				if (rvt != to->ival &&
				    AG_ResetTimer(ob, to, rvt) == -1) {
					rv = -1;
					AG_UnlockTimers(ob);
					goto out;
				}
			} else {				/* Cancel */
				AG_DelTimer(ob, to);
			}
			AG_UnlockTimers(ob);
			break;
		default:
			break;
		}
		if (agExitGLX) {
			goto out;
		}
		RenderDirtyWindows();
	}
out:
	if (rv != 0)
		Verbose("Abnormal exit: %s\n", AG_GetError());
}

#elif defined(HAVE_TIMERFD)

/*
 * Generic application event loop (using select() and Linux timerfd).
 */
static void
GLX_GenericEventLoop_TIMERFD(void *obj)
{
	AG_DriverEvent dev;
	int rv = 0, rvSelect, xfd, nfds;
	AG_Object *ob, *obNext;
	AG_Timer *to, *toNext;
	Uint32 rvt;
	fd_set rfds;

	if (agSoftTimers) {
		GLX_GenericEventLoop(obj);		/* Fallback */
		return;
	}
	AG_MutexLock(&agDisplayLock);
	xfd = ConnectionNumber(agDisplay);
	AG_MutexUnlock(&agDisplayLock);
	for (;;) {
		nfds = 0;
		FD_ZERO(&rfds);
		FD_SET(xfd, &rfds);
		if (xfd > nfds) { nfds = xfd; }
		AG_LockTiming();
		TAILQ_FOREACH(ob, &agTimerObjQ, tobjs) {
			AG_ObjectLock(ob);
			TAILQ_FOREACH(to, &ob->timers, timers) {
				FD_SET(to->id, &rfds);
				if (to->id > nfds) { nfds = to->id; }
			}
			AG_ObjectUnlock(ob);
		}
		AG_UnlockTiming();

		rvSelect = select(nfds+1, &rfds, NULL, NULL, NULL);
		if (rvSelect == -1) {
			if (errno == EINTR) {
				continue;
			}
			AG_SetError("select(): %s", AG_Strerror(errno));
			rv = -1;
			goto out;
		}
		if (FD_ISSET(xfd, &rfds)) {
			AG_LockVFS(&agDrivers);
			while (GLX_PendingEvents(NULL) != 0) {
				if (GLX_GetNextEvent(NULL, &dev) != 1) {
					break;
				}
				if (GLX_ProcessEvent(NULL, &dev) == -1) {
					AG_UnlockVFS(&agDrivers);
					goto out;
				}
			}
			AG_UnlockVFS(&agDrivers);
		}
		AG_LockTiming();
		for (ob = TAILQ_FIRST(&agTimerObjQ);
		     ob != TAILQ_END(&agTimerObjQ);
		     ob = obNext) {
			obNext = TAILQ_NEXT(ob, tobjs);
			AG_ObjectLock(ob);
			for (to = TAILQ_FIRST(&ob->timers);
			     to != TAILQ_END(&ob->timers);
			     to = toNext) {
				toNext = TAILQ_NEXT(to, timers);
				if (!FD_ISSET(to->id, &rfds)) {
					continue;
				}
				rvt = to->fn(to, &to->fnEvent);
				if (rvt > 0) {
					if (AG_ResetTimer(ob, to, rvt) == -1) {
						rv = -1;
						AG_ObjectUnlock(ob);
						AG_UnlockTiming();
						goto out;
					}
					break;
				} else {
					AG_DelTimer(ob, to);
				}
			}
			AG_ObjectUnlock(ob);
		}
		AG_UnlockTiming();

		if (agExitGLX) {
			goto out;
		}
		RenderDirtyWindows();
	}
out:
	if (rv != 0)
		Verbose("Abnormal exit: %s\n", AG_GetError());
}
#endif /* HAVE_TIMERFD */

static void
GLX_Terminate(void)
{
	AG_DriverGLX *glx;
	XClientMessageEvent xe;
	
	AG_MutexLock(&agDisplayLock);
	AGOBJECT_FOREACH_CHILD(glx, &agDrivers, ag_driver_glx) {
		if (!AGDRIVER_IS_GLX(glx)) {
			continue;
		}
		AG_MutexLock(&glx->lock);
		memset(&xe, 0, sizeof(XClientMessageEvent));
		xe.format = 32;
		xe.data.l[0] = glx->w;
		XSendEvent(agDisplay, glx->w, False, NoEventMask, (XEvent *)&xe);
		AG_MutexUnlock(&glx->lock);
	}
	XSync(agDisplay, False);
	agExitGLX = 1;
	AG_MutexUnlock(&agDisplayLock);
}

static void
GLX_BeginRendering(void *obj)
{
	AG_DriverGLX *glx = obj;

	AG_MutexLock(&agDisplayLock);
	glXMakeCurrent(agDisplay, glx->w, glx->glxCtx);
	AG_MutexUnlock(&agDisplayLock);
}

static void
GLX_RenderWindow(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	AG_GL_Context *gl = &glx->gl;
	AG_Color c = WCOLOR(win,0);

	gl->clipStates[0] = glIsEnabled(GL_CLIP_PLANE0); glEnable(GL_CLIP_PLANE0);
	gl->clipStates[1] = glIsEnabled(GL_CLIP_PLANE1); glEnable(GL_CLIP_PLANE1);
	gl->clipStates[2] = glIsEnabled(GL_CLIP_PLANE2); glEnable(GL_CLIP_PLANE2);
	gl->clipStates[3] = glIsEnabled(GL_CLIP_PLANE3); glEnable(GL_CLIP_PLANE3);

	glClearColor(c.r/255.0,
	             c.g/255.0,
		     c.b/255.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

	AG_WidgetDraw(win);
}

static void
GLX_EndRendering(void *obj)
{
	AG_DriverGLX *glx = obj;
	AG_GL_Context *gl = &glx->gl;
	Uint i;
	
	AG_MutexLock(&agDisplayLock);

	/* Exchange front and back buffers. */
	glXSwapBuffers(agDisplay, glx->w);

	/* Remove textures and display lists queued for deletion. */
	glDeleteTextures(gl->nTextureGC, (const GLuint *)gl->textureGC);
	for (i = 0; i < gl->nListGC; i++) {
		glDeleteLists(gl->listGC[i], 1);
	}
	gl->nTextureGC = 0;
	gl->nListGC = 0;

	AG_MutexUnlock(&agDisplayLock);
}

static int
GLX_SetRefreshRate(void *obj, int fps)
{
	if (fps < 1) {
		AG_SetError("Invalid refresh rate");
		return (-1);
	}
	AG_MutexLock(&agDisplayLock);
	rNom = 1000/fps;
	rCur = 0;
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

/*
 * Clipping and blending control (rendering context)
 */

/*
 * Cursor operations
 */

static int
GLX_CreateCursor(void *obj, AG_Cursor *ac)
{
	AG_DriverGLX *glx = obj;
	AG_CursorGLX *cg;
	int i, size;
	char *xData, *xMask;
	XGCValues gcVals;
	GC gc;
	XImage *dataImg, *maskImg;
	Pixmap dataPixmap, maskPixmap;

	if ((cg = TryMalloc(sizeof(AG_CursorGLX))) == NULL) {
		return (-1);
	}
	memset(&cg->black, 0, sizeof(cg->black));
	cg->white.pixel = 0xffff;
	cg->white.red = 0xffff;
	cg->white.green = 0xffff;
	cg->white.blue = 0xffff;

	size = (ac->w/8)*ac->h;
	if ((xData = TryMalloc(size)) == NULL) {
		Free(cg);
		return (-1);
	}
	if ((xMask = TryMalloc(size)) == NULL) {
		Free(xData);
		Free(cg);
		return (-1);
	}
	for (i = 0; i < size; i++) {
		xMask[i] = ac->data[i] | ac->mask[i];
		xData[i] = ac->data[i];
	}
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	/* Create the data image */
	dataImg = XCreateImage(agDisplay,
	    DefaultVisual(agDisplay,agScreen),
	    1, XYBitmap, 0, xData, ac->w,ac->h, 8, ac->w/8);
	dataImg->byte_order = MSBFirst;
	dataImg->bitmap_bit_order = MSBFirst;
	dataPixmap = XCreatePixmap(agDisplay,
	    RootWindow(agDisplay,agScreen),
	    ac->w,ac->h, 1);

	/* Create the mask image */
	maskImg = XCreateImage(agDisplay,
	    DefaultVisual(agDisplay,agScreen),
	    1, XYBitmap, 0, xMask, ac->w,ac->h, 8, ac->w/8);
	maskImg->byte_order = MSBFirst;
	maskImg->bitmap_bit_order = MSBFirst;
	maskPixmap = XCreatePixmap(agDisplay,
	    RootWindow(agDisplay,agScreen),
	    ac->w,ac->h,1);

	/* Blit the data and mask to the pixmaps */
	gcVals.function = GXcopy;
	gcVals.foreground = ~0;
	gcVals.background =  0;
	gcVals.plane_mask = AllPlanes;
	gc = XCreateGC(agDisplay, dataPixmap,
	    GCFunction|GCForeground|GCBackground|GCPlaneMask,
	    &gcVals);
	XPutImage(agDisplay, dataPixmap, gc, dataImg, 0,0, 0,0, ac->w,ac->h);
	XPutImage(agDisplay, maskPixmap, gc, maskImg, 0,0, 0,0, ac->w,ac->h);
	XFreeGC(agDisplay, gc);
	XDestroyImage(dataImg);
	XDestroyImage(maskImg);

	/* Create the X cursor */
	cg->xc = XCreatePixmapCursor(agDisplay, dataPixmap, maskPixmap,
	    &cg->black, &cg->white, ac->xHot, ac->yHot);
	cg->visible = 0;

	XFreePixmap(agDisplay, dataPixmap);
	XFreePixmap(agDisplay, maskPixmap);
	
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	
	XSync(agDisplay, False);

	ac->p = cg;
	return (0);
}

static void
GLX_FreeCursor(void *obj, AG_Cursor *ac)
{
	AG_DriverGLX *glx = obj;
	AG_CursorGLX *cg = ac->p;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);
	
	XFreeCursor(agDisplay, cg->xc);
	
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	
	XSync(agDisplay, False);

	Free(cg);
	ac->p = NULL;
}

static int
GLX_SetCursor(void *obj, AG_Cursor *ac)
{
	AG_Driver *drv = obj;
	AG_DriverGLX *glx = obj;
	AG_CursorGLX *cg = ac->p;

	if (drv->activeCursor == ac) {
		return (0);
	}

	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	if (ac == &drv->cursors[0]) {
		XUndefineCursor(agDisplay, glx->w);
	} else {
		XDefineCursor(agDisplay, glx->w, cg->xc);
	}

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	
	XSync(agDisplay, False);

	drv->activeCursor = ac;
	cg->visible = 1;
	return (0);
}

static void
GLX_UnsetCursor(void *obj)
{
	AG_Driver *drv = obj;
	AG_DriverGLX *glx = obj;
	
	if (drv->activeCursor == &drv->cursors[0])
		return;

	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	XUndefineCursor(agDisplay, glx->w);
	drv->activeCursor = &drv->cursors[0];
	
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	
	XSync(agDisplay, False);
}

static int
GLX_GetCursorVisibility(void *obj)
{
	/* XXX TODO */
	return (1);
}

static void
GLX_SetCursorVisibility(void *obj, int flag)
{
	/* XXX TODO */
}

/*
 * Window operations
 */

/* Wait on X events sent to the given window. */
static Bool
WaitConfigureNotify(Display *d, XEvent *xe, char *arg) {
	return (xe->type == ConfigureNotify) && (xe->xmap.window == (Window)arg);
}
static Bool
WaitMapNotify(Display *d, XEvent *xe, char *arg) {
	return (xe->type == MapNotify) && (xe->xmap.window == (Window)arg);
}
static Bool
WaitUnmapNotify(Display *d, XEvent *xe, char *arg) {
	return (xe->type == UnmapNotify) && (xe->xmap.window == (Window)arg);
}

/* Initialize the default cursor. */
static int
InitDefaultCursor(AG_DriverGLX *glx)
{
	AG_Driver *drv = AGDRIVER(glx);
	AG_Cursor *ac;
	AG_CursorGLX *cg;
	
	if ((cg = TryMalloc(sizeof(AG_CursorGLX))) == NULL)
		return (-1);
	if ((drv->cursors = TryMalloc(sizeof(AG_Cursor))) == NULL) {
		Free(cg);
		return (-1);
	}

	ac = &drv->cursors[0];
	drv->nCursors = 1;
	AG_CursorInit(ac);
	cg->xc = XCreateFontCursor(agDisplay, XC_left_ptr);
	cg->visible = 1;
	ac->p = cg;
	return (0);
}

/* Set WM_NORMAL_HINTS */
static void
SetWmNormalHints(AG_Window *win, const AG_Rect *r, Uint mwFlags)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	XSizeHints *h;

	/* Set WM_NORMAL_HINTS */
	if ((h = XAllocSizeHints()) == NULL)  {
		return;
	}
	h->flags = 0;
	if (!(mwFlags & AG_DRIVER_MW_ANYPOS))
		h->flags |= PPosition;

	h->flags |= PMinSize | PMaxSize;
	if (!(win->flags & AG_WINDOW_NORESIZE)) {
		h->min_width = 32;
		h->min_height = 32;
		h->max_width = 4096;
		h->max_height = 4096;
	} else {
		h->min_width = h->max_width = r->w;
		h->min_height = h->max_height = r->h;
	}
	XSetWMNormalHints(agDisplay, glx->w, h);
	XFree(h);
}

/*
 * Set the hints for Motif-compliant window managers.
 */
struct ag_motif_wm_hints {
	unsigned long flags;
	unsigned long fns;
	unsigned long dec;
	long input_mode;
	unsigned long status;
};
#define AG_MWM_HINTS_FUNCTIONS   (1L << 0)
#define AG_MWM_HINTS_DECORATIONS (1L << 1)
#define AG_MWM_HINTS_INPUT_MODE  (1L << 2)
#define AG_MWM_FN_ALL       (1L << 0)
#define AG_MWM_FN_RESIZE    (1L << 1)
#define AG_MWM_FN_MOVE      (1L << 2)
#define AG_MWM_FN_MINIMIZE  (1L << 3)
#define AG_MWM_FN_MAXIMIZE  (1L << 4)
#define AG_MWM_FN_CLOSE     (1L << 5)
#define AG_MWM_DEC_ALL      (1L << 0)
#define AG_MWM_DEC_BORDER   (1L << 1)
#define AG_MWM_DEC_RESIZEH  (1L << 2)
#define AG_MWM_DEC_TITLE    (1L << 3)
#define AG_MWM_DEC_MENU     (1L << 4)
#define AG_MWM_DEC_MINIMIZE (1L << 5)
#define AG_MWM_DEC_MAXIMIZE (1L << 6)
#define AG_MWM_INPUT_MODELESS                  0L	/* Document modal */
#define AG_MWM_INPUT_PRIMARY_APPLICATION_MODAL 1L	/* Document modal */
#define AG_MWM_INPUT_FULL_APPLICATION_MODAL    3L	/* Application modal */
static void
SetMotifWmHints(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	struct ag_motif_wm_hints hNew, *hints = NULL;
	Atom type = None;
	Ulong nItems, bytesAfter;
	Uchar *data;
	int format, rv;

	hNew.flags = AG_MWM_HINTS_FUNCTIONS|AG_MWM_HINTS_DECORATIONS|
	             AG_MWM_HINTS_INPUT_MODE;
	hNew.fns = 0UL;
	hNew.dec = 0UL;
	hNew.input_mode = (win->flags & AG_WINDOW_MODAL) ?
	                  AG_MWM_INPUT_FULL_APPLICATION_MODAL :
			  AG_MWM_INPUT_MODELESS;
	hNew.status = 0UL;

	if (!(win->flags & AG_WINDOW_NOMINIMIZE)) {
		hNew.fns |= AG_MWM_FN_MINIMIZE;
		hNew.dec |= AG_MWM_DEC_MINIMIZE;
	}
	if (!(win->flags & AG_WINDOW_NOMAXIMIZE)) {
		hNew.fns |= AG_MWM_FN_MAXIMIZE;
		hNew.dec |= AG_MWM_DEC_MAXIMIZE;
	}
	if (!(win->flags & AG_WINDOW_NORESIZE))  hNew.fns |= AG_MWM_FN_RESIZE;
	if (!(win->flags & AG_WINDOW_NOMOVE))    hNew.fns |= AG_MWM_FN_MOVE;
	if (!(win->flags & AG_WINDOW_NOCLOSE))   hNew.fns |= AG_MWM_FN_CLOSE;
	if (!(win->flags & AG_WINDOW_NOBORDERS)) hNew.dec |= AG_MWM_DEC_BORDER;
	if (!(win->flags & AG_WINDOW_NOHRESIZE)) hNew.dec |= AG_MWM_DEC_RESIZEH;
	if (!(win->flags & AG_WINDOW_NOTITLE))   hNew.dec |= AG_MWM_DEC_TITLE;

	rv = XGetWindowProperty(agDisplay, glx->w, wmMotifWmHints, 0,
	    sizeof(struct ag_motif_wm_hints)/sizeof(long), False,
	    AnyPropertyType, &type, &format, &nItems, &bytesAfter, &data);
	if (rv != Success)
		return;

	if (data == NULL || type != wmMotifWmHints) {
		hints = &hNew;
	} else {
		hints = (struct ag_motif_wm_hints *)data;
		hints->flags = hNew.flags;
		hints->fns = hNew.fns;
		hints->dec = hNew.dec;
		hints->input_mode = hNew.input_mode;
	}
	XChangeProperty(agDisplay, glx->w,
	    wmMotifWmHints, wmMotifWmHints, 32,
	    PropModeReplace,
	    (Uchar *)hints,
	    sizeof(struct ag_motif_wm_hints)/sizeof(long));
}

/* Set hints for KWM */
static void
SetKwmWmHints(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;

	if (win->flags & AG_WINDOW_NOTITLE ||
	    win->flags & AG_WINDOW_NOBORDERS) {
		long KWMHints = 0;

		XChangeProperty(agDisplay, glx->w,
		    wmKwmWinDecoration, wmKwmWinDecoration, 32,
		    PropModeReplace,
		    (Uchar *)&KWMHints,
		    sizeof(KWMHints)/sizeof(long));
	}
}

/* Set hints for GNOME */
static void
SetGnomeWmHints(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;

	if (win->flags & AG_WINDOW_NOTITLE ||
	    win->flags & AG_WINDOW_NOBORDERS) {
		long GNOMEHints = 0;

		XChangeProperty(agDisplay, glx->w,
		    wmWinHints, wmWinHints, 32,
		    PropModeReplace,
		    (Uchar *)&GNOMEHints,
		    sizeof(GNOMEHints)/sizeof(long));
	}
}

/* Set hints for AG_WINDOW_MODAL windows. */
static void
SetModalHints(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;

	if (wmNetWmState != None && wmNetWmStateModal != None) {
		XChangeProperty(agDisplay, glx->w,
		    wmNetWmState, XA_ATOM, 32,
		    PropModeAppend,
		    (Uchar *)&wmNetWmStateModal, 1);
	}
}

/* Set hints for AG_WINDOW_KEEPABOVE/KEEPBELOW settings. */
static void
SetAboveBelowHints(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;

	if (win->flags & AG_WINDOW_KEEPABOVE) {
		if (wmNetWmState != None &&
		    wmNetWmStateAbove != None) {
			XChangeProperty(agDisplay, glx->w,
			    wmNetWmState, XA_ATOM, 32,
			    PropModeAppend,
			    (Uchar *)&wmNetWmStateAbove, 1);
		}
	} else if (win->flags & AG_WINDOW_KEEPBELOW) {
		if (wmNetWmState != None &&
		    wmNetWmStateBelow != None) {
			XChangeProperty(agDisplay, glx->w,
			    wmNetWmState, XA_ATOM, 32,
			    PropModeAppend,
			    (Uchar *)&wmNetWmStateBelow, 1);
		}
	}
}

static int
GLX_OpenWindow(AG_Window *win, AG_Rect r, int depthReq, Uint mwFlags)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	AG_Driver *drv = WIDGET(win)->drv;
	XSetWindowAttributes xwAttrs;
	XVisualInfo *xvi;
	int glxAttrs[16];
	int i = 0;
	Ulong valuemask;
	int depth;
	
	glxAttrs[i++] = GLX_RGBA;
	glxAttrs[i++] = GLX_RED_SIZE;	glxAttrs[i++] = 2;
	glxAttrs[i++] = GLX_GREEN_SIZE;	glxAttrs[i++] = 2;
	glxAttrs[i++] = GLX_BLUE_SIZE;	glxAttrs[i++] = 2;
	glxAttrs[i++] = GLX_DEPTH_SIZE;	glxAttrs[i++] = 16;
	glxAttrs[i++] = GLX_DOUBLEBUFFER;
	if (agStereo) { glxAttrs[i++] = GLX_STEREO; }
	glxAttrs[i++] = None;

	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	if ((xvi = glXChooseVisual(agDisplay, agScreen, glxAttrs)) == NULL) {
		glxAttrs[i-=2] = None;
		if ((xvi = glXChooseVisual(agDisplay, agScreen, glxAttrs))
		    == NULL) {
			AG_SetError("Cannot find an acceptable GLX visual");
			goto fail_unlock;
		}
	}

	xwAttrs.colormap = XCreateColormap(agDisplay,
	    RootWindow(agDisplay,xvi->screen),
	    xvi->visual, AllocNone);
	xwAttrs.background_pixmap = None;
	xwAttrs.border_pixel = (xvi->visual == DefaultVisual(agDisplay,agScreen)) ?
	                       BlackPixel(agDisplay,agScreen) : 0;
	xwAttrs.event_mask = KeyPressMask|KeyReleaseMask|
	                     ButtonPressMask|ButtonReleaseMask|
			     EnterWindowMask|LeaveWindowMask|
			     PointerMotionMask|ButtonMotionMask|
			     KeymapStateMask|
			     StructureNotifyMask|
			     FocusChangeMask|
			     ExposureMask;
	valuemask = CWColormap | CWBackPixmap | CWBorderPixel | CWEventMask;

	/* Create a new window. */
	depth = (depthReq >= 1) ? depthReq : xvi->depth;
	glx->w = XCreateWindow(agDisplay,
	    RootWindow(agDisplay,agScreen),
	    r.x, r.y,
	    r.w, r.h, 0, depth,
	    InputOutput,
	    xvi->visual,
	    valuemask,
	    &xwAttrs);
	if (glx->w == 0) {
		AG_SetError("XCreateWindow failed");
		goto fail_unlock;
	}

	/* Honor AG_WindowMakeTransient() */
	if (win->transientFor != NULL)
		GLX_SetTransientFor(win, win->transientFor);
	
	/*
	 * Set WM_NORMAL_HINTS now (other WM hints will be passed in
	 * the MapWindow() function).
	 */
	SetWmNormalHints(win, &r, mwFlags);

	/* Create the GLX rendering context. */
	glx->glxCtx = glXCreateContext(agDisplay, xvi, 0, GL_TRUE);
	glXMakeCurrent(agDisplay, glx->w, glx->glxCtx);
	if (AG_GL_InitContext(glx, &glx->gl) == -1) {
		goto fail_win;
	}
	AG_GL_SetViewport(&glx->gl, AG_RECT(0, 0, WIDTH(win), HEIGHT(win)));
	
	/* Set the pixel formats. */
	drv->videoFmt = AG_PixelFormatRGB(depth,
#if AG_BYTEORDER == AG_BIG_ENDIAN
		0xff000000, 0x00ff0000, 0x0000ff00
#else
		0x000000ff, 0x0000ff00, 0x00ff0000
#endif
	);
	if (drv->videoFmt == NULL)
		goto fail_ctx;

	/* Create the built-in cursors. */
	if (InitDefaultCursor(glx) == -1 ||
	    AG_InitStockCursors(drv) == -1) {
		goto fail_ctx;
	}
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);

	XFree(xvi);
	return (0);
fail_ctx:
	AG_GL_DestroyContext(glx);
	glXMakeCurrent(agDisplay, None, NULL);
	glXDestroyContext(agDisplay, glx->glxCtx);
fail_win:
	XDestroyWindow(agDisplay, glx->w);
	if (drv->videoFmt) {
		AG_PixelFormatFree(drv->videoFmt);
		drv->videoFmt = NULL;
	}
fail_unlock:
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	if (xvi)
		XFree(xvi);
	return (-1);
}

static void
GLX_CloseWindow(AG_Window *win)
{
	AG_Driver *drv = WIDGET(win)->drv;
	AG_DriverGLX *glx = (AG_DriverGLX *)drv;

	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	/* Destroy our OpenGL context. */
	glXMakeCurrent(agDisplay, glx->w, glx->glxCtx);
	AG_GL_DestroyContext(glx);
	glXMakeCurrent(agDisplay, None, NULL);
	glXDestroyContext(agDisplay, glx->glxCtx);

	/* Destroy the window. */
	XDestroyWindow(agDisplay, glx->w);
	if (drv->videoFmt) {
		AG_PixelFormatFree(drv->videoFmt);
		drv->videoFmt = NULL;
	}

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
}

static int
GLX_MapWindow(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	XEvent xev;
	AG_SizeAlloc a;
	int x, y;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	/* Set the window manager hints. */
	if (!glx->wmHintsSet) {
		Atom wmprot[2], atom;
		int nwmprot;
		
		glx->wmHintsSet = 1;
	
		/* Set EWMH-compliant window type */
		if (wmNetWmWindowType != None) {
			atom = XInternAtom(agDisplay,
			    agWindowWmTypeNames[win->wmType], True);
			if (atom != None) {
				XChangeProperty(agDisplay, glx->w,
				    wmNetWmWindowType, XA_ATOM, 32,
				    PropModeReplace,
				    (Uchar *)&atom, 1);
			}
			if (win->wmType == AG_WINDOW_WM_DROPDOWN_MENU ||
			    win->wmType == AG_WINDOW_WM_POPUP_MENU) {
				atom = XInternAtom(agDisplay,
				    "_NET_WM_WINDOW_TYPE_MENU", True);
				if (atom != None) {
					XChangeProperty(agDisplay, glx->w,
					    wmNetWmWindowType, XA_ATOM, 32,
					    PropModeAppend,
					    (Uchar *)&atom, 1);
				}
			}
			if (win->wmType != AG_WINDOW_WM_NORMAL) {
				atom = XInternAtom(agDisplay,
				    "_NET_WM_WINDOW_TYPE_DOCK", True);
				if (atom != None) {
					XChangeProperty(agDisplay, glx->w,
					    wmNetWmWindowType, XA_ATOM, 32,
					    PropModeAppend,
					    (Uchar *)&atom, 1);
				}
			}
		}
		if (win->wmType != AG_WINDOW_WM_NORMAL &&
		    wmNetWmState != None &&
		    wmNetWmStateSkipTaskbar != None) {
			XChangeProperty(agDisplay, glx->w,
			    wmNetWmState, XA_ATOM, 32,
			    PropModeAppend,
			    (Uchar *)&wmNetWmStateSkipTaskbar, 1);
		}
	
		/* Set window manager hints. */
		if (wmMotifWmHints != None)
			SetMotifWmHints(win);
		if (wmKwmWinDecoration != None)
			SetKwmWmHints(win);
		if (wmWinHints != None)
			SetGnomeWmHints(win);

		/* Set application-modal window hints. */
		if (win->flags & AG_WINDOW_MODAL)
			SetModalHints(win);
	
		/* Honor KEEPABOVE and KEEPBELOW */
		if (win->flags & AG_WINDOW_KEEPABOVE ||
		    win->flags & AG_WINDOW_KEEPBELOW)
			SetAboveBelowHints(win);

		/* Honor NOCLOSE and DENYFOCUS */
		nwmprot = 0;
		if (!(win->flags & AG_WINDOW_NOCLOSE))
			wmprot[nwmprot++] = wmDeleteWindow;
		if (win->flags & AG_WINDOW_DENYFOCUS) {
			XWMHints h;
			h.flags = InputHint;
			h.input = False;
			XSetWMHints(agDisplay, glx->w, &h);
			wmprot[nwmprot++] = wmTakeFocus;
		}
		XChangeProperty(agDisplay, glx->w,
		    wmProtocols, XA_ATOM, 32,
		    PropModeReplace,
		    (Uchar *)wmprot, nwmprot);
	}

	XMapWindow(agDisplay, glx->w);
	XIfEvent(agDisplay, &xev, WaitMapNotify, (char *)glx->w);

	/* Update per-widget coordinate information. */
	x = WIDGET(win)->x;
	y = WIDGET(win)->y;
	a.x = 0;
	a.y = 0;
	a.w = WIDTH(win);
	a.h = HEIGHT(win);
	AG_WidgetSizeAlloc(win, &a);
	AG_WidgetUpdateCoords(win, 0, 0);
	WIDGET(win)->x = x;
	WIDGET(win)->y = y;

	/* Set the GL viewport. */
	glXMakeCurrent(agDisplay, glx->w, glx->glxCtx);
	AG_GL_SetViewport(&glx->gl, AG_RECT(0, 0, WIDTH(win), HEIGHT(win)));

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

static int
GLX_UnmapWindow(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	XEvent xev;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	XUnmapWindow(agDisplay, glx->w);
	XIfEvent(agDisplay, &xev, WaitUnmapNotify, (char *)glx->w);
	
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

static int
GLX_RaiseWindow(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	XRaiseWindow(agDisplay, glx->w);

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

static int
GLX_LowerWindow(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	XLowerWindow(agDisplay, glx->w);

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

static int
GLX_ReparentWindow(AG_Window *win, AG_Window *winParent, int x, int y)
{
	AG_DriverGLX *glxWin = (AG_DriverGLX *)WIDGET(win)->drv;
	AG_DriverGLX *glxParentWin = (AG_DriverGLX *)WIDGET(winParent)->drv;
/*	XEvent xev; */
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glxWin->lock);
	AG_MutexLock(&glxParentWin->lock);

	XReparentWindow(agDisplay, glxWin->w, glxParentWin->w, x,y);

	AG_MutexUnlock(&glxParentWin->lock);
	AG_MutexUnlock(&glxWin->lock);
	AG_MutexUnlock(&agDisplayLock);

/*	XIfEvent(agDisplay, &xev, WaitConfigureNotify, (char *)glx->w); */
	return (0);
}

static int
GLX_GetInputFocus(AG_Window **rv)
{
	AG_DriverGLX *glx = NULL;
	Window wRet;
	int revertToRet;

	AG_MutexLock(&agDisplayLock);
	XGetInputFocus(agDisplay, &wRet, &revertToRet);
	AG_MutexUnlock(&agDisplayLock);

	AGOBJECT_FOREACH_CHILD(glx, &agDrivers, ag_driver_glx) {
		if (!AGDRIVER_IS_GLX(glx)) {
			continue;
		}
		if (glx->w == wRet)
			break;
	}
	if (glx == NULL) {
		AG_SetError("Input focus is external to this application");
		return (-1);
	}
	*rv = AGDRIVER_MW(glx)->win;
	return (0);
}

static int
GLX_SetInputFocus(AG_Window *win)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;

	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	XSetInputFocus(agDisplay, glx->w, RevertToParent, CurrentTime);

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

static void
GLX_PreResizeCallback(AG_Window *win)
{
#if 0
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;

	/*
	 * Backup all GL resources since it is not portable to assume that a
	 * display resize will not cause a change in GL contexts
	 * (XXX TODO test for platforms where this is unnecessary)
	 * (XXX is this correctly done?)
	 */
	glXMakeCurrent(agDisplay, glx->w, glx->glxCtx);
	GLX_FreeWidgetResources(WIDGET(win));
	AG_TextClearGlyphCache(glx);
#endif
}

static void
GLX_PostResizeCallback(AG_Window *win, AG_SizeAlloc *a)
{
	AG_Driver *drv = WIDGET(win)->drv;
	AG_DriverGLX *glx = (AG_DriverGLX *)drv;
	AG_SizeAlloc aNew;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	/* Update the window size and coordinates. */
	aNew.x = 0;
	aNew.y = 0;
	aNew.w = a->w;
	aNew.h = a->h;
	AG_WidgetSizeAlloc(win, &aNew);
	AG_WidgetUpdateCoords(win, 0, 0);
	WIDGET(win)->x = a->x;
	WIDGET(win)->y = a->y;
	win->dirty = 1;

	/* Update the viewport. */
	glXMakeCurrent(agDisplay, glx->w, glx->glxCtx);
	AG_GL_SetViewport(&glx->gl, AG_RECT(0, 0, WIDTH(win), HEIGHT(win)));
	
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
}

static void
GLX_PostMoveCallback(AG_Window *win, AG_SizeAlloc *a)
{
	AG_Driver *drv = WIDGET(win)->drv;
	AG_DriverGLX *glx = (AG_DriverGLX *)drv;
	AG_SizeAlloc aNew;
	int xRel, yRel;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	xRel = a->x - WIDGET(win)->x;
	yRel = a->y - WIDGET(win)->y;

	/* Update the window coordinates. */
	aNew.x = 0;
	aNew.y = 0;
	aNew.w = a->w;
	aNew.h = a->h;
	AG_WidgetSizeAlloc(win, &aNew);
	AG_WidgetUpdateCoords(win, 0, 0);
	WIDGET(win)->x = a->x;
	WIDGET(win)->y = a->y;
	win->dirty = 1;

	/* Move other windows pinned to this one. */
	AG_WindowMovePinned(win, xRel, yRel);

	AG_MutexUnlock(&agDisplayLock);
	AG_MutexUnlock(&glx->lock);
}

static int
GLX_MoveWindow(AG_Window *win, int x, int y)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	XEvent xev;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	/* printf("XMoveWindow(%d,%d)\n", x, y); */
	XMoveWindow(agDisplay, glx->w, x, y);
	XIfEvent(agDisplay, &xev, WaitConfigureNotify, (char *)glx->w);

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);

	return (0);
}

#if 0
/* Save/restore associated widget GL resources (for GL context changes). */
static void
GLX_FreeWidgetResources(AG_Widget *wid)
{
	AG_Widget *chld;

	OBJECT_FOREACH_CHILD(chld, wid, ag_widget) {
		GLX_FreeWidgetResources(chld);
	}
	AG_WidgetFreeResourcesGL(wid);
}
#endif

#if 0
static void
RegenWidgetResources(AG_Widget *wid)
{
	AG_Widget *chld;

	OBJECT_FOREACH_CHILD(chld, wid, ag_widget) {
		RegenWidgetResources(chld);
	}
	AG_WidgetRegenResourcesGL(wid);
}
#endif

static int
GLX_ResizeWindow(AG_Window *win, Uint w, Uint h)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	XEvent xev;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);
	
	/* Resize the window and wait for notification from the server. */
	XResizeWindow(agDisplay, glx->w, w, h);
	XIfEvent(agDisplay, &xev, WaitConfigureNotify, (char *)glx->w);
	
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

static int
GLX_MoveResizeWindow(AG_Window *win, AG_SizeAlloc *a)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	XMoveResizeWindow(agDisplay, glx->w,
	    a->x, a->y,
	    a->w, a->h);

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

static int
GLX_SetBorderWidth(AG_Window *win, Uint width)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	XEvent xev;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

	XSetWindowBorderWidth(agDisplay, glx->w, width);
	XIfEvent(agDisplay, &xev, WaitConfigureNotify, (char *)glx->w);
	
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
}

static int
GLX_SetWindowCaption(AG_Window *win, const char *s)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	XTextProperty xtp;
	Status rv;

	if (s[0] == '\0') {
		return (0);
	}

	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);

#ifdef X_HAVE_UTF8_STRING
	/* XFree86 "utf8" and "UTF8" functions are available */
	rv = Xutf8TextListToTextProperty(agDisplay,
	    (char **)&s, 1, XUTF8StringStyle,
	    &xtp);
#else
	rv = XStringListToTextProperty(
	    (char **)&s, 1,
	    &xtp);
#endif
	if (rv != Success) {
		AG_SetError("Cannot convert string to X property");
		goto fail;
	}
	XSetTextProperty(agDisplay, glx->w, &xtp, XA_WM_NAME);
	XFree(xtp.value);
	XSync(agDisplay, False);

	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (0);
fail:
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
	return (-1);
}

static void
GLX_SetTransientFor(AG_Window *win, AG_Window *winParent)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	AG_DriverGLX *glxParent;
	
	AG_MutexLock(&agDisplayLock);
	AG_MutexLock(&glx->lock);
	
	if (winParent != NULL &&
	    (glxParent = (AG_DriverGLX *)WIDGET(winParent)->drv) != NULL &&
	    AGDRIVER_IS_GLX(glxParent) &&
	    (AGDRIVER_MW(glxParent)->flags & AG_DRIVER_MW_OPEN)) {
		XSetTransientForHint(agDisplay, glx->w, glxParent->w);
	} else {
		XSetTransientForHint(agDisplay, glx->w, None);
	}
	
	AG_MutexUnlock(&glx->lock);
	AG_MutexUnlock(&agDisplayLock);
}

static int
GLX_SetOpacity(AG_Window *win, float f)
{
	AG_DriverGLX *glx = (AG_DriverGLX *)WIDGET(win)->drv;
	unsigned long opacity = (f > 0.99) ? 0xffffffff : f*0xffffffff;

	if (wmNetWmWindowOpacity != None) {
		XChangeProperty(agDisplay, glx->w,
		    wmNetWmWindowOpacity, XA_CARDINAL, 32,
		    PropModeReplace,
		    (Uchar *)&opacity, 1);
		return (0);
	} else {
		AG_SetError("Opacity not implemented");
		return (-1);
	}
}

static void
GLX_TweakAlignment(AG_Window *win, AG_SizeAlloc *a, Uint wMax, Uint hMax)
{
	/* XXX TODO */
	switch (win->alignment) {
	case AG_WINDOW_TL:
	case AG_WINDOW_TC:
	case AG_WINDOW_TR:
		a->y += 50;
		if (a->y+a->h > hMax) { a->y = 0; }
		break;
	case AG_WINDOW_BL:
	case AG_WINDOW_BC:
	case AG_WINDOW_BR:
		a->y -= 100;
		if (a->y < 0) { a->y = 0; }
		break;
	default:
		break;
	}
}

AG_DriverMwClass agDriverGLX = {
	{
		{
			"AG_Driver:AG_DriverMw:AG_DriverGLX",
			sizeof(AG_DriverGLX),
			{ 1,4 },
			Init,
			NULL,	/* reinit */
			Destroy,
			NULL,	/* load */
			NULL,	/* save */
			NULL,	/* edit */
		},
		"glx",
		AG_VECTOR,
		AG_WM_MULTIPLE,
		AG_DRIVER_OPENGL|AG_DRIVER_TEXTURES,
		GLX_Open,
		GLX_Close,
		GLX_GetDisplaySize,
		NULL,			/* beginEventProcessing */
		GLX_PendingEvents,
		GLX_GetNextEvent,
		GLX_ProcessEvent,
#if defined(HAVE_KQUEUE)
		GLX_GenericEventLoop_KQUEUE,
#elif defined(HAVE_TIMERFD)
		GLX_GenericEventLoop_TIMERFD,
#else
		GLX_GenericEventLoop,
#endif
		NULL,			/* endEventProcessing */
		GLX_Terminate,
		GLX_BeginRendering,
		GLX_RenderWindow,
		GLX_EndRendering,
		AG_GL_FillRect,
		NULL,			/* updateRegion */
		AG_GL_StdUploadTexture,
		AG_GL_StdUpdateTexture,
		AG_GL_StdDeleteTexture,
		GLX_SetRefreshRate,
		AG_GL_StdPushClipRect,
		AG_GL_StdPopClipRect,
		AG_GL_StdPushBlendingMode,
		AG_GL_StdPopBlendingMode,
		GLX_CreateCursor,
		GLX_FreeCursor,
		GLX_SetCursor,
		GLX_UnsetCursor,
		GLX_GetCursorVisibility,
		GLX_SetCursorVisibility,
		AG_GL_BlitSurface,
		AG_GL_BlitSurfaceFrom,
		AG_GL_BlitSurfaceGL,
		AG_GL_BlitSurfaceFromGL,
		AG_GL_BlitSurfaceFlippedGL,
		AG_GL_BackupSurfaces,
		AG_GL_RestoreSurfaces,
		AG_GL_RenderToSurface,
		AG_GL_PutPixel,
		AG_GL_PutPixel32,
		AG_GL_PutPixelRGB,
		AG_GL_BlendPixel,
		AG_GL_DrawLine,
		AG_GL_DrawLineH,
		AG_GL_DrawLineV,
		AG_GL_DrawLineBlended,
		AG_GL_DrawArrowUp,
		AG_GL_DrawArrowDown,
		AG_GL_DrawArrowLeft,
		AG_GL_DrawArrowRight,
		AG_GL_DrawBoxRounded,
		AG_GL_DrawBoxRoundedTop,
		AG_GL_DrawCircle,
		AG_GL_DrawCircleFilled,
		AG_GL_DrawRectFilled,
		AG_GL_DrawRectBlended,
		AG_GL_DrawRectDithered,
		AG_GL_UpdateGlyph,
		AG_GL_DrawGlyph,
		AG_GL_StdDeleteList
	},
	GLX_OpenWindow,
	GLX_CloseWindow,
	GLX_MapWindow,
	GLX_UnmapWindow,
	GLX_RaiseWindow,
	GLX_LowerWindow,
	GLX_ReparentWindow,
	GLX_GetInputFocus,
	GLX_SetInputFocus,
	GLX_MoveWindow,
	GLX_ResizeWindow,
	GLX_MoveResizeWindow,
	GLX_PreResizeCallback,
	GLX_PostResizeCallback,
	NULL,				/* captureWindow */
	GLX_SetBorderWidth,
	GLX_SetWindowCaption,
	GLX_SetTransientFor,
	GLX_SetOpacity,
	GLX_TweakAlignment
};
