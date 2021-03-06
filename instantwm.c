/* See LICENSE file for copyright and license details.
 *
 * dynamic window manager is designed like any other X client as well. It is
 * driven through handling X events. In contrast to other X clients, a window
 * manager selects for SubstructureRedirectMask on the root window, to receive
 * events about window (dis-)appearance. Only one X connection at a time is
 * allowed to select for this event mask.
 *
 * The event handlers of instantWM are organized in an array which is accessed
 * whenever a new event has been fetched. This allows event dispatching
 * in O(1) time.
 *
 * Each child of the root window is called a client, except windows which have
 * set the override_redirect flag. Clients are organized in a linked client
 * list on each monitor, the focus history is remembered through a stack list
 * on each monitor. Each client contains a bit array to indicate the tags of a
 * client.
 *
 * Keys and tagging rules are organized as arrays and defined in config.h.
 *
 * To understand everything else, start reading main().
 */
#include <errno.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <X11/Xproto.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */
#include <X11/Xft/Xft.h>

#include "drw.h"
#include "util.h"

/* macros */
#define BUTTONMASK              (ButtonPressMask|ButtonReleaseMask)
#define CLEANMASK(mask)         (mask & ~(numlockmask|LockMask) & (ShiftMask|ControlMask|Mod1Mask|Mod2Mask|Mod3Mask|Mod4Mask|Mod5Mask))
#define INTERSECT(x,y,w,h,m)    (MAX(0, MIN((x)+(w),(m)->wx+(m)->ww) - MAX((x),(m)->wx)) \
                               * MAX(0, MIN((y)+(h),(m)->wy+(m)->wh) - MAX((y),(m)->wy)))
#define ISVISIBLE(C)            ((C->tags & C->mon->tagset[C->mon->seltags]) || C->issticky)
#define HIDDEN(C)               ((getstate(C->win) == IconicState))
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define MOUSEMASK               (BUTTONMASK|PointerMotionMask)
#define WIDTH(X)                ((X)->w + 2 * (X)->bw)
#define HEIGHT(X)               ((X)->h + 2 * (X)->bw)
#define TAGMASK                 ((1 << LENGTH(tags)) - 1)
#define TEXTW(X)                (drw_fontset_getwidth(drw, (X)) + lrpad)

#define MWM_HINTS_FLAGS_FIELD       0
#define MWM_HINTS_DECORATIONS_FIELD 2
#define MWM_HINTS_DECORATIONS       (1 << 1)
#define MWM_DECOR_ALL               (1 << 0)
#define MWM_DECOR_BORDER            (1 << 1)
#define MWM_DECOR_TITLE             (1 << 3)



#define SYSTEM_TRAY_REQUEST_DOCK    0

/* XEMBED messages */
#define XEMBED_EMBEDDED_NOTIFY      0
#define XEMBED_WINDOW_ACTIVATE      1
#define XEMBED_FOCUS_IN             4
#define XEMBED_MODALITY_ON         10

#define XEMBED_MAPPED              (1 << 0)
#define XEMBED_WINDOW_ACTIVATE      1
#define XEMBED_WINDOW_DEACTIVATE    2

#define VERSION_MAJOR               0
#define VERSION_MINOR               0
#define XEMBED_EMBEDDED_VERSION (VERSION_MAJOR << 16) | VERSION_MINOR

/* enums */
enum { CurNormal, CurResize, CurMove, CurClick, CurHor, CurVert, CurTL, CurTR, CurBL, CurBR, CurLast }; /* cursor */
enum { SchemeNorm, SchemeSel, SchemeHid, SchemeTags, SchemeActive, SchemeAddActive, SchemeEmpty, SchemeHover, SchemeClose, SchemeHoverTags }; /* color schemes */
enum { NetSupported, NetWMName, NetWMState, NetWMCheck,
       NetSystemTray, NetSystemTrayOP, NetSystemTrayOrientation, NetSystemTrayOrientationHorz,
       NetWMFullscreen, NetActiveWindow, NetWMWindowType,
       NetWMWindowTypeDialog, NetClientList, NetLast }; /* EWMH atoms */
enum { Manager, Xembed, XembedInfo, XLast }; /* Xembed atoms */
enum { WMProtocols, WMDelete, WMState, WMTakeFocus, WMLast }; /* default atoms */
enum { ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle,
       ClkClientWin, ClkRootWin, ClkCloseButton, ClkShutDown, ClkSideBar, ClkStartMenu, ClkLast }; /* clicks */

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int click;
	unsigned int mask;
	unsigned int button;
	void (*func)(const Arg *arg);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct Client Client;
struct Client {
	char name[256];
	float mina, maxa;
	int x, y, w, h;
	int sfx, sfy, sfw, sfh; /* stored float geometry, used on mode revert */
	int oldx, oldy, oldw, oldh;
	int basew, baseh, incw, inch, maxw, maxh, minw, minh;
	int bw, oldbw;
	unsigned int tags;
	int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen, isfakefullscreen, islocked, issticky;
	Client *next;
	Client *snext;
	Monitor *mon;
	Window win;
};

typedef struct {
	unsigned int mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

typedef struct Pertag Pertag;
struct Monitor {
	char ltsymbol[16];
	float mfact;
	int nmaster;
	int num;
	int by;               /* bar geometry */
	int btw;              /* width of tasks portion of bar */
	int bt;               /* number of tasks */
	int mx, my, mw, mh;   /* screen size */
	int wx, wy, ww, wh;   /* window area  */
	unsigned int seltags;
	unsigned int sellt;
	unsigned int tagset[2];
	unsigned int activeoffset;
	unsigned int titleoffset;
	int showbar;
	int topbar;
	Client *clients;
	Client *sel;
	Client *overlay;
	int overlaystatus;
	int gesture;
	Client *stack;
	Client *hoverclient;
	Monitor *next;
	Window barwin;
	const Layout *lt[2];
	unsigned int showtags;
	Pertag *pertag;
};

typedef struct {
	const char *class;
	const char *instance;
	const char *title;
	unsigned int tags;
	int isfloating;
	int monitor;
} Rule;

typedef struct Systray   Systray;
struct Systray {
	Window win;
	Client *icons;
};

/* function declarations */
static void applyrules(Client *c);
static int applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact);
static void arrange(Monitor *m);
static void arrangemon(Monitor *m);
static void resetcursor();
static void attach(Client *c);
static void attachstack(Client *c);
static void buttonpress(XEvent *e);
static void checkotherwm(void);
static void cleanup(void);
static void cleanupmon(Monitor *mon);
static void clientmessage(XEvent *e);
static void configure(Client *c);
static void configurenotify(XEvent *e);
static void configurerequest(XEvent *e);
static Monitor *createmon(void);
static void cyclelayout(const Arg *arg);
static void destroynotify(XEvent *e);
static void detach(Client *c);
static void detachstack(Client *c);
static Monitor *dirtomon(int dir);
static void drawbar(Monitor *m);
static void drawbars(void);
static int drawstatusbar(Monitor *m, int bh, char* text);
static void enternotify(XEvent *e);
static void expose(XEvent *e);
static void focus(Client *c);
static void focusin(XEvent *e);
static void focusmon(const Arg *arg);
static void focusstack(const Arg *arg);
static Atom getatomprop(Client *c, Atom prop);
static int getrootptr(int *x, int *y);
static long getstate(Window w);
static unsigned int getsystraywidth();
static int gettextprop(Window w, Atom atom, char *text, unsigned int size);
static void grabbuttons(Client *c, int focused);
static void grabkeys(void);
static void hide(Client *c);
static void incnmaster(const Arg *arg);
static void keypress(XEvent *e);
static void killclient(const Arg *arg);
static void manage(Window w, XWindowAttributes *wa);
static void mappingnotify(XEvent *e);
static void maprequest(XEvent *e);
static void monocle(Monitor *m);
static void motionnotify(XEvent *e);
static void movemouse(const Arg *arg);
static void dragmouse(const Arg *arg);
static void gesturemouse(const Arg *arg);
static void dragrightmouse(const Arg *arg);
static void drawwindow(const Arg *arg);
static void waitforclickend(const Arg *arg);
static void dragtag(const Arg *arg);
static void moveresize(const Arg *arg);
static void distributeclients(const Arg *arg);
static void keyresize(const Arg *arg);
static void centerwindow();
static Client *nexttiled(Client *c);
static void pop(Client *);
static void propertynotify(XEvent *e);
static void quit(const Arg *arg);
static Monitor *recttomon(int x, int y, int w, int h);
static void removesystrayicon(Client *i);
static void resize(Client *c, int x, int y, int w, int h, int interact);
static void resizebarwin(Monitor *m);
static void resizeclient(Client *c, int x, int y, int w, int h);
static void resizemouse(const Arg *arg);
static void resizeaspectmouse(const Arg *arg);
static void resizerequest(XEvent *e);
static void restack(Monitor *m);
static void animateclient(Client *c, int x, int y, int w, int h, int frames, int resetpos);
static void run(void);
static void runAutostart(void);
static void scan(void);
static int sendevent(Window w, Atom proto, int m, long d0, long d1, long d2, long d3, long d4);
static void sendmon(Client *c, Monitor *m);
static int gettagwidth();
static int getxtag(int ix);
static void setclientstate(Client *c, long state);
static void setfocus(Client *c);
static void setfullscreen(Client *c, int fullscreen);
static void setlayout(const Arg *arg);
static void setmfact(const Arg *arg);
static void setup(void);
static void seturgent(Client *c, int urg);
static void show(Client *c);
static void showhide(Client *c);
static void sigchld(int unused);
static void spawn(const Arg *arg);
static Monitor *systraytomon(Monitor *m);
static void tag(const Arg *arg);
static void followtag(const Arg *arg);
static void followview(const Arg *arg);
static void tagmon(const Arg *arg);
static void tagtoleft(const Arg *arg);
static void tagtoright(const Arg *arg);
static void tile(Monitor *);
static void togglealttag(const Arg *arg);
static void toggleanimated(const Arg *arg);
static void toggledoubledraw(const Arg *arg);
static void togglefakefullscreen(const Arg *arg);
static void togglelocked(const Arg *arg);
static void toggleshowtags();
static void togglebar(const Arg *arg);
static void togglefloating(const Arg *arg);
static void togglesticky(const Arg *arg);
static void toggletag(const Arg *arg);
static void toggleview(const Arg *arg);
static void hidewin(const Arg *arg);
static void unhideall(const Arg *arg);
static void closewin(const Arg *arg);
static void unfocus(Client *c, int setfocus);
static void unmanage(Client *c, int destroyed);
static void unmapnotify(XEvent *e);
static void updatebarpos(Monitor *m);
static void updatebars(void);
static void updateclientlist(void);
static int updategeom(void);
static void updatemotifhints(Client *c);
static void updatenumlockmask(void);
static void updatesizehints(Client *c);
static void updatestatus(void);
static void updatesystray(void);
static void updatesystrayicongeom(Client *i, int w, int h);
static void updatesystrayiconstate(Client *i, XPropertyEvent *ev);
static void updatetitle(Client *c);
static void updatewindowtype(Client *c);
static void updatewmhints(Client *c);
static void view(const Arg *arg);
static void warp(const Client *c);
static void forcewarp(const Client *c);
static void warpfocus();
static void viewtoleft(const Arg *arg);
static void animleft(const Arg *arg);
static void animright(const Arg *arg);
static void moveleft(const Arg *arg);
static void viewtoright(const Arg *arg);
static void moveright(const Arg *arg);

static void overtoggle(const Arg *arg);
static void fullovertoggle(const Arg *arg);

static Client *wintoclient(Window w);
static Monitor *wintomon(Window w);
static Client *wintosystrayicon(Window w);
static void winview(const Arg* arg);

static int xerror(Display *dpy, XErrorEvent *ee);
static int xerrordummy(Display *dpy, XErrorEvent *ee);
static int xerrorstart(Display *dpy, XErrorEvent *ee);
static void zoom(const Arg *arg);

static void bstack(Monitor *m);
static void bstackhoriz(Monitor *m);


static void keyrelease(XEvent *e);
static void setoverlay();
static void desktopset();
static void createdesktop();
static void createoverlay();
static void shiftview(const Arg *arg);

/* variables */
static Systray *systray =  NULL;
static const char broken[] = "broken";
static char stext[1024];

static int showalttag = 0;
static int animated = 1;
static int bardragging = 0;
static int altcursor = 0;
static int tagwidth = 0;
static int doubledraw = 0;
static int desktopicons = 0;
static int newdesktop = 0;

static int statuswidth = 0;
static int topdrag = 0;

static int isdesktop = 0;

static int screen;
static int sw, sh;           /* X display screen geometry width, height */
static int bh, blw = 0;      /* bar geometry */
static int lrpad;            /* sum of left and right padding for text */
static int (*xerrorxlib)(Display *, XErrorEvent *);
static unsigned int numlockmask = 0;
static void (*handler[LASTEvent]) (XEvent *) = {
	[ButtonPress] = buttonpress,
	[ButtonRelease] = keyrelease,
	[ClientMessage] = clientmessage,
	[ConfigureRequest] = configurerequest,
	[ConfigureNotify] = configurenotify,
	[DestroyNotify] = destroynotify,
	[EnterNotify] = enternotify,
	[Expose] = expose,
	[FocusIn] = focusin,
	[KeyRelease] = keyrelease,
	[KeyPress] = keypress,
	[MappingNotify] = mappingnotify,
	[MapRequest] = maprequest,
	[MotionNotify] = motionnotify,
	[PropertyNotify] = propertynotify,
	[ResizeRequest] = resizerequest,
	[UnmapNotify] = unmapnotify
};
static Atom wmatom[WMLast], netatom[NetLast], xatom[XLast], motifatom;
static int running = 1;
static Cur *cursor[CurLast];
static Clr **scheme;
static Display *dpy;
static Drw *drw;
static Monitor *mons, *selmon;
static Window root, wmcheckwin;

/* configuration, allows nested code to access above variables */
#include "config.h"

struct Pertag {
	unsigned int curtag, prevtag; /* current and previous tag */
	int nmasters[LENGTH(tags) + 1]; /* number of windows in master area */
	float mfacts[LENGTH(tags) + 1]; /* mfacts per tag */
	unsigned int sellts[LENGTH(tags) + 1]; /* selected layouts */
	const Layout *ltidxs[LENGTH(tags) + 1][2]; /* matrix of tags and layouts indexes  */
	int showbars[LENGTH(tags) + 1]; /* display bar for the current tag */
};

/* compile-time check if all tags fit into an unsigned int bit array. */
struct NumTags { char limitexceeded[LENGTH(tags) > 31 ? -1 : 1]; };

/* function implementations */
static int combo = 0;

void
keyrelease(XEvent *e) {
	combo = 0;
}

int overlayexists() {
	Client *c;

	if (!selmon->overlay)
		return 0;
	for(c = selmon->clients; c; c = c->next) {
		if (c == selmon->overlay) {
			return 1;
		}
	}
	
	return 0;
}

void createdesktop(){
	Client *c;
	Monitor *m;
	m = selmon;
	for(c = m->clients; c; c = c->next) {
		if (strstr(c->name, "ROX-Filer") != NULL) {
			if (c->w > drw->w - 100) {
				focus(c);
				desktopset();
				break;
			}
		}
	}
}

void
createoverlay() {
	if (!selmon->sel)
		return;
	if (selmon->sel == selmon->overlay) {
		if (!selmon->overlay->isfloating) {
			changefloating(selmon->overlay);
		}
		resize(selmon->sel, selmon->mx + 20, bh, selmon->ww - 40, (selmon->wh) / 3, True);
		arrange(selmon);
		return;
	}

	Client *tempclient = selmon->sel;
	
	selmon->overlaystatus = 1;

	resetoverlay();

	selmon->overlay = tempclient;
	tempclient->bw = 0;
	tempclient->islocked = 1;
	if (!selmon->overlay->isfloating) {
		changefloating(selmon->overlay);
	}

	selmon->overlay->h =((selmon->wh) / 3);
	showoverlay();
}

void
resetoverlay() {
	if (!overlayexists())
		return;
	selmon->overlay->tags = selmon->tagset[selmon->seltags];
	selmon->overlay->bw = borderpx;
	selmon->overlay->islocked = 0;
	changefloating(selmon->overlay);
	arrange(selmon);
	focus(selmon->overlay);

}

double easeOutQuint( double t ) {
    return 1 + (--t) * t * t;
}

// move client to position within a set amount of frames
void animateclient(Client *c, int x, int y, int w, int h, int frames, int resetpos)
{
	int time;
	int oldx, oldy;
	int width, height;
	width = w ? w : c->w;
	height = h ? h : c->h;
	time = 1;
	oldx = c->x;
	oldy = c->y;

	if (animated && (abs(oldx - x) > 10 || abs(oldy - y) > 10 || abs(w - c->w) > 10 || abs(h - c->h) > 10)) {
		if (x == c->x && y == c->y && c->w < selmon->mw - 50) {
			animateclient(c, c->x + (width - c->w), c->y + (height - c->h), 0, 0, frames, 0);
		} else {
			while (time < frames)
			{
				resize(c,
					oldx + easeOutQuint(((double)time/frames)) * (x - oldx),
					oldy + easeOutQuint(((double)time/frames)) * (y - oldy), width, height, 1);
				time++;
				usleep(15000);
			}
		}
	}

	if (resetpos)
		resize(c, oldx, oldy, width, height, 0);
	else
		resize(c, x, y, width, height, 1);

}

void
showoverlay() {
	if (!overlayexists())
		return;
	selmon->overlaystatus = 1;
	Client *c = selmon->overlay;

	if (c->islocked) {
		if (selmon->showbar)
			resize(c, selmon->mx + 20, bh - c->h, selmon->ww - 40, c->h, True);
		else
			resize(c, selmon->mx + 20, 0, selmon->ww - 40, c->h, True);
	}
	
	c->tags = selmon->tagset[selmon->seltags];
	focus(c);

	if (!c->isfloating) {
		changefloating(selmon->overlay);
	}


	if (c->islocked)
	{
		XRaiseWindow(dpy, c->win);
		if (selmon->showbar)
			animateclient(c, c->x, bh, 0, 0, 15, 0);
		else
			animateclient(c, c->x, 0, 0, 0, 15, 0);
		c->issticky = 1;

	}

	c->bw = 0;
	arrange(selmon);
}

void
hideoverlay() {
	if (!overlayexists())
		return;
	
	Client *c;
	c = selmon->overlay;
	c->issticky = 0;

	if (c->islocked)
		animateclient(c, c->x, 0 - c->h, 0, 0, 15, 0);

	selmon->overlaystatus = 0;
	selmon->overlay->tags = 0;
	focus(NULL);
	arrange(selmon);

}

void
setoverlay() {
	
	if (!overlayexists()) {
		return;
	}

	if (!selmon->overlaystatus) {
		showoverlay();
	} else {
		if (ISVISIBLE(selmon->overlay)) {
			hideoverlay();
		} else {
			showoverlay();
		}
	}
}

void desktopset() {
	Client *c = selmon->sel;
	c->isfloating = 0;
	arrange(c->mon);
	resize(c, 0,bh,drw->w, drw->h - bh, 0);
	unmanage(c, 0);
	restack(selmon);
	return;
}

void
applyrules(Client *c)
{
	const char *class, *instance;
	unsigned int i;
	const Rule *r;
	Monitor *m;
	XClassHint ch = { NULL, NULL };

	/* rule matching */
	c->isfloating = 0;
	c->tags = 0;
	XGetClassHint(dpy, c->win, &ch);
	class    = ch.res_class ? ch.res_class : broken;
	instance = ch.res_name  ? ch.res_name  : broken;

	for (i = 0; i < LENGTH(rules); i++) {
		r = &rules[i];
		if ((!r->title || strstr(c->name, r->title))
		&& (!r->class || strstr(class, r->class))
		&& (!r->instance || strstr(instance, r->instance)))
		{
			if (strstr(r->class, "ROX-Filer") != NULL) {
				desktopicons = 1;
				newdesktop = 1;
			}
			c->isfloating = r->isfloating;
			c->tags |= r->tags;
			for (m = mons; m && m->num != r->monitor; m = m->next);
			if (m)
				c->mon = m;
		}
	}
	if (ch.res_class)
		XFree(ch.res_class);
	if (ch.res_name)
		XFree(ch.res_name);
	c->tags = c->tags & TAGMASK ? c->tags & TAGMASK : c->mon->tagset[c->mon->seltags];
}

int
applysizehints(Client *c, int *x, int *y, int *w, int *h, int interact)
{
	Monitor *m = c->mon;

	/* set minimum possible */
	*w = MAX(1, *w);
	*h = MAX(1, *h);
	if (interact) {
		if (*x > sw)
			*x = sw - WIDTH(c);
		if (*y > sh)
			*y = sh - HEIGHT(c);
		if (*x + *w + 2 * c->bw < 0)
			*x = 0;
		if (*y + *h + 2 * c->bw < 0)
			*y = 0;
	} else {
		if (*x >= m->wx + m->ww)
			*x = m->wx + m->ww - WIDTH(c);
		if (*y >= m->wy + m->wh)
			*y = m->wy + m->wh - HEIGHT(c);
		if (*x + *w + 2 * c->bw <= m->wx)
			*x = m->wx;
		if (*y + *h + 2 * c->bw <= m->wy)
			*y = m->wy;
	}
	if (*h < bh)
		*h = bh;
	if (*w < bh)
		*w = bh;
	if (resizehints || c->isfloating || !c->mon->lt[c->mon->sellt]->arrange) {
		/* see last two sentences in ICCCM 4.1.2.3 */
		int baseismin = c->basew == c->minw && c->baseh == c->minh;
		if (!baseismin) { /* temporarily remove base dimensions */
			*w -= c->basew;
			*h -= c->baseh;
		}
		/* adjust for aspect limits */
		if (c->mina > 0 && c->maxa > 0) {
			if (c->maxa < (float)*w / *h)
				*w = *h * c->maxa + 0.5;
			else if (c->mina < (float)*h / *w)
				*h = *w * c->mina + 0.5;
		}
		if (baseismin) { /* increment calculation requires this */
			*w -= c->basew;
			*h -= c->baseh;
		}
		/* adjust for increment value */
		if (c->incw)
			*w -= *w % c->incw;
		if (c->inch)
			*h -= *h % c->inch;
		/* restore base dimensions */
		*w = MAX(*w + c->basew, c->minw);
		*h = MAX(*h + c->baseh, c->minh);
		if (c->maxw)
			*w = MIN(*w, c->maxw);
		if (c->maxh)
			*h = MIN(*h, c->maxh);
	}
	return *x != c->x || *y != c->y || *w != c->w || *h != c->h;
}

void
arrange(Monitor *m)
{
	resetcursor();
	if (m)
		showhide(m->stack);
	else for (m = mons; m; m = m->next)
		showhide(m->stack);
	if (m) {
		arrangemon(m);
		restack(m);
	} else for (m = mons; m; m = m->next)
		arrangemon(m);
}

void
arrangemon(Monitor *m)
{
	strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, sizeof m->ltsymbol);
	if (m->lt[m->sellt]->arrange)
		m->lt[m->sellt]->arrange(m);
}

void
attach(Client *c)
{
	c->next = c->mon->clients;
	c->mon->clients = c;
}

void
attachstack(Client *c)
{
	c->snext = c->mon->stack;
	c->mon->stack = c;
}

void resetcursor()
{
	if (!altcursor)
		return;
	XDefineCursor(dpy, root, cursor[CurNormal]->cursor);
	altcursor = 0;
}

void
buttonpress(XEvent *e)
{
	unsigned int i, x, click, occ = 0;
	Arg arg = {0};
	Client *c;
	Monitor *m;
	XButtonPressedEvent *ev = &e->xbutton;

	click = ClkRootWin;
	/* focus monitor if necessary */
	if ((m = wintomon(ev->window)) && m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}

	if (ev->window == selmon->barwin) {
		i = 0;
		x = startmenusize;
		for (c = m->clients; c; c = c->next)
			occ |= c->tags == 255 ? 0 : c->tags;
		do {
			/* do not reserve space for vacant tags */
			if (selmon->showtags){
				if (!(occ & 1 << i || m->tagset[m->seltags] & 1 << i))
					continue;
			}

			x += TEXTW(tags[i]);	
		} while (ev->x >= x && ++i < LENGTH(tags));
		if (ev->x < startmenusize) {
			click = ClkStartMenu;
			selmon->gesture = 0;
			drawbar(selmon);
		} else if (i < LENGTH(tags)) {
			click = ClkTagBar;
			arg.ui = 1 << i;
		} else if (ev->x < x + blw)
			click = ClkLtSymbol;
		else if (!selmon->sel && ev->x > x + blw &&  ev->x < x + blw + bh)
			click = ClkShutDown;
		/* 2px right padding */
		else if (ev->x > selmon->ww - getsystraywidth() - statuswidth + lrpad - 2)
			click = ClkStatusText;
		else {
			if (selmon->stack) {
				x += blw;
				c = m->clients;

				do {
					if (!ISVISIBLE(c))
						continue;
					else
						x += (1.0 / (double)m->bt) * m->btw;
				} while (ev->x > x && (c = c->next));

				if (c) {
					arg.v = c;
					if (c != selmon->sel || ev->x > x - (1.0 / (double)m->bt) * m->btw + 32) {
						click = ClkWinTitle;
					} else {
						click = ClkCloseButton;
					}
				} 
			} else {
				click = ClkRootWin;
			}
		}
	} else if ((c = wintoclient(ev->window))) {
		focus(c);
		restack(selmon);
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		click = ClkClientWin;
	} else if (ev->x > selmon->mx + selmon->mw - 50) {
		click = ClkSideBar;
	}
	for (i = 0; i < LENGTH(buttons); i++)
		if (click == buttons[i].click && buttons[i].func && buttons[i].button == ev->button
		&& CLEANMASK(buttons[i].mask) == CLEANMASK(ev->state))
			buttons[i].func((click == ClkTagBar || click == ClkWinTitle || click == ClkCloseButton || click == ClkShutDown || click == ClkSideBar) && buttons[i].arg.i == 0 ? &arg : &buttons[i].arg);
}

void
checkotherwm(void)
{
	xerrorxlib = XSetErrorHandler(xerrorstart);
	/* this causes an error if some other window manager is running */
	XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);
	XSync(dpy, False);
	XSetErrorHandler(xerror);
	XSync(dpy, False);
}

void
cleanup(void)
{
	Arg a = {.ui = ~0};
	Layout foo = { "", NULL };
	Monitor *m;
	size_t i;

	view(&a);
	selmon->lt[selmon->sellt] = &foo;
	for (m = mons; m; m = m->next)
		while (m->stack)
			unmanage(m->stack, 0);
	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	while (mons)
		cleanupmon(mons);
	if (showsystray) {
		XUnmapWindow(dpy, systray->win);
		XDestroyWindow(dpy, systray->win);
		free(systray);
	}
	for (i = 0; i < CurLast; i++)
		drw_cur_free(drw, cursor[i]);
	for (i = 0; i < LENGTH(colors) + 1; i++)
		free(scheme[i]);
	XDestroyWindow(dpy, wmcheckwin);
	drw_free(drw);
	XSync(dpy, False);
	XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);
	XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
}

void
cleanupmon(Monitor *mon)
{
	Monitor *m;

	if (mon == mons)
		mons = mons->next;
	else {
		for (m = mons; m && m->next != mon; m = m->next);
		m->next = mon->next;
	}
	XUnmapWindow(dpy, mon->barwin);
	XDestroyWindow(dpy, mon->barwin);
	free(mon);
}

void
clientmessage(XEvent *e)
{
	XWindowAttributes wa;
	XSetWindowAttributes swa;
	XClientMessageEvent *cme = &e->xclient;
	Client *c = wintoclient(cme->window);
	unsigned int i;

	if (showsystray && cme->window == systray->win && cme->message_type == netatom[NetSystemTrayOP]) {
		/* add systray icons */
		if (cme->data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
			if (!(c = (Client *)calloc(1, sizeof(Client))))
				die("fatal: could not malloc() %u bytes\n", sizeof(Client));
			if (!(c->win = cme->data.l[2])) {
				free(c);
				return;
			}
			c->mon = selmon;
			c->next = systray->icons;
			systray->icons = c;
			XGetWindowAttributes(dpy, c->win, &wa);
			c->x = c->oldx = c->y = c->oldy = 0;
			c->w = c->oldw = wa.width;
			c->h = c->oldh = wa.height;
			c->oldbw = wa.border_width;
			c->bw = 0;
			c->isfloating = True;
			/* reuse tags field as mapped status */
			c->tags = 1;
			updatesizehints(c);
			updatesystrayicongeom(c, wa.width, wa.height);
			XAddToSaveSet(dpy, c->win);
			XSelectInput(dpy, c->win, StructureNotifyMask | PropertyChangeMask | ResizeRedirectMask);
			XReparentWindow(dpy, c->win, systray->win, 0, 0);
			/* use parents background color */
			swa.background_pixel  = scheme[SchemeNorm][ColBg].pixel;
			XChangeWindowAttributes(dpy, c->win, CWBackPixel, &swa);
			sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_EMBEDDED_NOTIFY, 0 , systray->win, XEMBED_EMBEDDED_VERSION);
			/* FIXME not sure if I have to send these events, too */
			sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_FOCUS_IN, 0 , systray->win, XEMBED_EMBEDDED_VERSION);
			sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_WINDOW_ACTIVATE, 0 , systray->win, XEMBED_EMBEDDED_VERSION);
			sendevent(c->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_MODALITY_ON, 0 , systray->win, XEMBED_EMBEDDED_VERSION);
			XSync(dpy, False);
			resizebarwin(selmon);
			updatesystray();
			setclientstate(c, NormalState);
		}
		return;
	}
	if (!c)
		return;
	if (cme->message_type == netatom[NetWMState]) {
		if (cme->data.l[1] == netatom[NetWMFullscreen]
		|| cme->data.l[2] == netatom[NetWMFullscreen])
			setfullscreen(c, (cme->data.l[0] == 1 /* _NET_WM_STATE_ADD    */
				|| (cme->data.l[0] == 2 /* _NET_WM_STATE_TOGGLE */ && (!c->isfullscreen || c->isfakefullscreen))));
	} else if (cme->message_type == netatom[NetActiveWindow]) {
		if (c == selmon->overlay) {
			showoverlay();
		} else {
			for (i = 0; i < LENGTH(tags) && !((1 << i) & c->tags); i++);
			if (i < LENGTH(tags)) {
				const Arg a = {.ui = 1 << i};
				if (selmon != c->mon) {
					unfocus(selmon->sel, 0);
					selmon = c->mon;
				}
				view(&a);
				focus(c);
				restack(selmon);
			}
		}

	}
}

void
configure(Client *c)
{
	XConfigureEvent ce;

	ce.type = ConfigureNotify;
	ce.display = dpy;
	ce.event = c->win;
	ce.window = c->win;
	ce.x = c->x;
	ce.y = c->y;
	ce.width = c->w;
	ce.height = c->h;
	ce.border_width = c->bw;
	ce.above = None;
	ce.override_redirect = False;
	XSendEvent(dpy, c->win, False, StructureNotifyMask, (XEvent *)&ce);
}

void
configurenotify(XEvent *e)
{
	Monitor *m;
	Client *c;
	XConfigureEvent *ev = &e->xconfigure;

	/* TODO: updategeom handling sucks, needs to be simplified */
	if (ev->window == root) {
		int dirty = (sw != ev->width || sh != ev->height);
		sw = ev->width;
		sh = ev->height;
		if (updategeom() || dirty) {
			drw_resize(drw, sw, bh);
			updatebars();
			for (m = mons; m; m = m->next) {
				if (c->isfakefullscreen){
					XMoveResizeWindow(dpy, m->barwin, m->wx, m->by, m->ww, bh);
				}else{
					for (c = m->clients; c; c = c->next)
						if (c->isfullscreen)
							resizeclient(c, m->mx, m->my, m->mw, m->mh);
					resizebarwin(m);
				}

			}
			focus(NULL);
			arrange(NULL);
		}
	}
}

void distributeclients(const Arg *arg) {
	Client *c;
	int tagcounter = 0;
	focus(NULL);

	for (c = selmon->clients; c; c = c->next) {
		if (c == selmon->overlay)
			continue;
		if (tagcounter > 8) {
			tagcounter = 0;
		}
		if (c && 1<<tagcounter & TAGMASK) {
			c->tags = 1<<tagcounter & TAGMASK;
		}
		tagcounter++;
	}
	focus(NULL);
	arrange(selmon);
}

void
configurerequest(XEvent *e)
{
	Client *c;
	Monitor *m;
	XConfigureRequestEvent *ev = &e->xconfigurerequest;
	XWindowChanges wc;

	if ((c = wintoclient(ev->window))) {
		if (ev->value_mask & CWBorderWidth)
			c->bw = ev->border_width;
		else if (c->isfloating || !selmon->lt[selmon->sellt]->arrange) {
			m = c->mon;
			if (ev->value_mask & CWX) {
				c->oldx = c->x;
				c->x = m->mx + ev->x;
			}
			if (ev->value_mask & CWY) {
				c->oldy = c->y;
				c->y = m->my + ev->y;
			}
			if (ev->value_mask & CWWidth) {
				c->oldw = c->w;
				c->w = ev->width;
			}
			if (ev->value_mask & CWHeight) {
				c->oldh = c->h;
				c->h = ev->height;
			}
			if ((c->x + c->w) > m->mx + m->mw && c->isfloating)
				c->x = m->mx + (m->mw / 2 - WIDTH(c) / 2); /* center in x direction */
			if ((c->y + c->h) > m->my + m->mh && c->isfloating)
				c->y = m->my + (m->mh / 2 - HEIGHT(c) / 2); /* center in y direction */
			if ((ev->value_mask & (CWX|CWY)) && !(ev->value_mask & (CWWidth|CWHeight)))
				configure(c);
			if (ISVISIBLE(c))
				XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
		} else
			configure(c);
	} else {
		wc.x = ev->x;
		wc.y = ev->y;
		wc.width = ev->width;
		wc.height = ev->height;
		wc.border_width = ev->border_width;
		wc.sibling = ev->above;
		wc.stack_mode = ev->detail;
		XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
	}
	XSync(dpy, False);
}

Monitor *
createmon(void)
{
	Monitor *m;
	unsigned int i;

	m = ecalloc(1, sizeof(Monitor));
	m->tagset[0] = m->tagset[1] = 1;
	m->mfact = mfact;
	m->nmaster = nmaster;
	m->showbar = showbar;
	m->topbar = topbar;
	m->lt[0] = &layouts[3];
	m->lt[1] = &layouts[0];
	strncpy(m->ltsymbol, layouts[0].symbol, sizeof m->ltsymbol);
	m->pertag = ecalloc(1, sizeof(Pertag));
	m->pertag->curtag = m->pertag->prevtag = 1;

	for (i = 0; i <= LENGTH(tags); i++) {
		m->pertag->nmasters[i] = m->nmaster;
		m->pertag->mfacts[i] = m->mfact;

		m->pertag->ltidxs[i][0] = m->lt[1];
		m->pertag->ltidxs[i][1] = m->lt[0];
		m->pertag->sellts[i] = m->sellt;

		m->pertag->showbars[i] = m->showbar;
	}

	return m;
}

void
cyclelayout(const Arg *arg) {
	Layout *l;
	for(l = (Layout *)layouts; l != selmon->lt[selmon->sellt]; l++);
	if(arg->i > 0) {
		if(l->symbol && (l + 1)->symbol)
			setlayout(&((Arg) { .v = (l + 1) }));
		else
			setlayout(&((Arg) { .v = layouts }));
	} else {
		if(l != layouts && (l - 1)->symbol)
			setlayout(&((Arg) { .v = (l - 1) }));
		else
			setlayout(&((Arg) { .v = &layouts[LENGTH(layouts) - 2] }));
	}
}

void
destroynotify(XEvent *e)
{
	Client *c;
	XDestroyWindowEvent *ev = &e->xdestroywindow;

	if ((c = wintoclient(ev->window)))
		unmanage(c, 1);
	else if ((c = wintosystrayicon(ev->window))) {
		removesystrayicon(c);
		resizebarwin(selmon);
		updatesystray();
	}
}

void
detach(Client *c)
{
	Client **tc;

	for (tc = &c->mon->clients; *tc && *tc != c; tc = &(*tc)->next);
	*tc = c->next;
}

void
detachstack(Client *c)
{
	Client **tc, *t;

	for (tc = &c->mon->stack; *tc && *tc != c; tc = &(*tc)->snext);
	*tc = c->snext;

	if (c == c->mon->sel) {
		for (t = c->mon->stack; t && !ISVISIBLE(t); t = t->snext);
		c->mon->sel = t;
	}
}

Monitor *
dirtomon(int dir)
{
	Monitor *m = NULL;

	if (dir > 0) {
		if (!(m = selmon->next))
			m = mons;
	} else if (selmon == mons)
		for (m = mons; m->next; m = m->next);
	else
		for (m = mons; m->next != selmon; m = m->next);
	return m;
}

int
drawstatusbar(Monitor *m, int bh, char* stext) {
	int ret, i, w, x, len;
	short isCode;
	char *text;
	char *p;

	len = strlen(stext) + 1 ;
	if (!(text = (char*) malloc(sizeof(char)*len)))
		die("malloc");
	p = text;
	memcpy(text, stext, len);

	/* compute width of the status text */
	w = 0;
	i = -1;
	while (text[++i]) {
		if (text[i] == '^') {
			if (!isCode) {
				isCode = 1;
				text[i] = '\0';
				w += TEXTW(text) - lrpad;
				text[i] = '^';
				if (text[++i] == 'f')
					w += atoi(text + ++i);
			} else {
				isCode = 0;
				text = text + i + 1;
				i = -1;
			}
		}
	}
	if (!isCode)
		w += TEXTW(text) - lrpad;
	else
		isCode = 0;
	text = p;
	statuswidth = w;
	w += 2; /* 1px padding on both sides */
	ret = x = m->ww - w - getsystraywidth();

	drw_setscheme(drw, scheme[LENGTH(colors)]);
	drw->scheme[ColFg] = scheme[SchemeNorm][ColFg];
	drw_rect(drw, x, 0, w, bh, 1, 1);
	x++;

	/* process status text */
	i = -1;
	while (text[++i]) {
		if (text[i] == '^' && !isCode) {
			isCode = 1;

			text[i] = '\0';
			w = TEXTW(text) - lrpad;
			drw_text(drw, x, 0, w, bh, 0, text, 0, 0);

			x += w;

			/* process code */
			while (text[++i] != '^') {
				if (text[i] == 'c') {
					char buf[8];
					memcpy(buf, (char*)text+i+1, 7);
					buf[7] = '\0';
					drw_clr_create(drw, &drw->scheme[ColBg], buf);
					i += 7;
				} else if (text[i] == 'd') {
					drw->scheme[ColBg] = scheme[SchemeNorm][ColBg];
				} else if (text[i] == 'r') {
					int rx = atoi(text + ++i);
					while (text[++i] != ',');
					int ry = atoi(text + ++i);
					while (text[++i] != ',');
					int rw = atoi(text + ++i);
					while (text[++i] != ',');
					int rh = atoi(text + ++i);

					drw_rect(drw, rx + x, ry, rw, rh, 1, 0);
				} else if (text[i] == 'f') {
					x += atoi(text + ++i);
				}
			}

			text = text + i + 1;
			i=-1;
			isCode = 0;
		}
	}

	if (!isCode) {
		w = TEXTW(text) - lrpad;
		drw_text(drw, x, 0, w, bh, 0, text, 0, 0);
	}

	drw_setscheme(drw, scheme[SchemeNorm]);
	free(p);

	return ret;
}

void
drawbar(Monitor *m)
{

	int x, w, sw = 0, n = 0, stw = 0, scm, wdelta, roundw;
    unsigned int i, occ = 0, urg = 0;
	Client *c;

	if(showsystray && m == systraytomon(m))
		stw = getsystraywidth();

	/* draw status first so it can be overdrawn by tags later */
	if (m == selmon) { /* status is only drawn on selected monitor */
		sw = m->ww - stw - drawstatusbar(m, bh, stext);

	}

	//draw start menu icon

	int startmenuinvert = (selmon->gesture == 13);
	drw_rect(drw, 0, 0, startmenusize, bh, 1, startmenuinvert ? 0:1);
	drw_rect(drw, 5, 5, 14, 14, 1, startmenuinvert ? 1:0);
	drw_rect(drw, 9, 9, 6, 6, 1, startmenuinvert ? 0:1);
	drw_rect(drw, 19, 19, 6, 6, 1, startmenuinvert ? 1:0);

	resizebarwin(m);
	for (c = m->clients; c; c = c->next) {
		if (ISVISIBLE(c))
			n++;
		occ |= c->tags == 255 ? 0 : c->tags;

		if (c->isurgent)
			urg |= c->tags;
	}
	x = startmenusize;
	for (i = 0; i < LENGTH(tags); i++) {

		/* do not draw vacant tags */
		if (selmon->showtags) {
			if (!(occ & 1 << i || m->tagset[m->seltags] & 1 << i))
			continue;
		}

		w = TEXTW(tags[i]);
		wdelta = showalttag ? abs(TEXTW(tags[i]) - TEXTW(tagsalt[i])) / 2 : 0;

		if (occ & 1 << i) {
			if (m == selmon && selmon->sel && selmon->sel->tags & 1 << i) {
				drw_setscheme(drw, scheme[SchemeActive]);
			} else {
				if (m->tagset[m->seltags] & 1 << i) {
					drw_setscheme(drw, scheme[SchemeAddActive]);
				} else {
					if(!selmon->showtags){
						drw_setscheme(drw, scheme[SchemeTags]);
					} else {
						drw_setscheme(drw, scheme[SchemeNorm]);
					}
				}
			}
		} else {
			if (m->tagset[m->seltags] & 1 << i) {
				drw_setscheme(drw, scheme[SchemeEmpty]);
			} else {
				drw_setscheme(drw, scheme[SchemeNorm]);
			}
		}

		if (i == selmon->gesture - 1) {
			roundw = 8;
			if (bardragging) {
				drw_setscheme(drw, scheme[SchemeHoverTags]);
			} else {
				if (drw->scheme == scheme[SchemeTags]) {
							drw_setscheme(drw, scheme[SchemeHoverTags]);
				} else if (drw->scheme == scheme[SchemeNorm]) {
					drw_setscheme(drw, scheme[SchemeHover]);
					roundw = 2;
				}
			}

			drw_text(drw, x, 0, w, bh, lrpad / 2, (showalttag ? tagsalt[i] : tags[i]), urg & 1 << i, roundw);

		} else {
				drw_text(drw, x, 0, w, bh, lrpad / 2, (showalttag ? tagsalt[i] : tags[i]), urg & 1 << i, drw->scheme == scheme[SchemeNorm] ? 0 : 4);
		
		}
		x += w;
	}
	w = blw = 60;
	drw_setscheme(drw, scheme[SchemeNorm]);
	x = drw_text(drw, x, 0, w, bh, (w - TEXTW(m->ltsymbol)) * 0.5 + 10, m->ltsymbol, 0, 0);

	if ((w = m->ww - sw - x - stw) > bh) {
		if (n > 0) {
			for (c = m->clients; c; c = c->next) {
				if (!ISVISIBLE(c))
					continue;
				if (m->sel == c) {

					//background color rectangles to draw circle on
					if (!c->issticky)
						drw_setscheme(drw, scheme[SchemeTags]);
					else
						drw_setscheme(drw, scheme[SchemeActive]);

					if (TEXTW(c->name) < (1.0 / (double)n) * w - 64){
						drw_text(drw, x, 0, (1.0 / (double)n) * w, bh, ((1.0 / (double)n) * w - TEXTW(c->name)) * 0.5, c->name, 0, 4);
					} else {
						drw_text(drw, x, 0, (1.0 / ((double)n) * w), bh, lrpad / 2 + 20, c->name, 0, 4);
					}

					// render close button
					if (!c->islocked) {
						drw_setscheme(drw, scheme[SchemeClose]);
						if (selmon->gesture != 12) {
							XSetForeground(drw->dpy, drw->gc, drw->scheme[ColBg].pixel);
							XFillRectangle(drw->dpy, drw->drawable, drw->gc, x +  6, 4, 20, 16);
							XSetForeground(drw->dpy, drw->gc, drw->scheme[ColFloat].pixel);
							XFillRectangle(drw->dpy, drw->drawable, drw->gc, x + 6, 20, 20, 4);
						} else {
							XSetForeground(drw->dpy, drw->gc, drw->scheme[ColFg].pixel);
							XFillRectangle(drw->dpy, drw->drawable, drw->gc, x +  6, 2, 20, 16);
							XSetForeground(drw->dpy, drw->gc, drw->scheme[ColBg].pixel);
							XFillRectangle(drw->dpy, drw->drawable, drw->gc, x + 6, 18, 20, 6);
						}
					} else {
						drw_setscheme(drw, scheme[SchemeAddActive]);
						XSetForeground(drw->dpy, drw->gc, drw->scheme[ColBg].pixel);
						XFillRectangle(drw->dpy, drw->drawable, drw->gc, x +  6, 4, 20, 16);
						XSetForeground(drw->dpy, drw->gc, drw->scheme[ColFloat].pixel);
						XFillRectangle(drw->dpy, drw->drawable, drw->gc, x + 6, 20, 20, 4);

					}

					m->activeoffset = selmon->mx + x;

				x += (1.0 / (double)n) * w;
					
				} else {
					if (HIDDEN(c)) {
						scm = SchemeHid;
					} else{
						if (!c->issticky)
							scm = SchemeNorm;
						else
							scm = SchemeAddActive;
					}
					drw_setscheme(drw, scheme[scm]);
					if (TEXTW(c->name) < (1.0 / (double)n) * w){
						drw_text(drw, x, 0, (1.0 / (double)n) * w, bh, ((1.0 / (double)n) * w - TEXTW(c->name)) * 0.5, c->name, 0, 0);
					} else {
						drw_text(drw, x, 0, (1.0 / (double)n) * w, bh, lrpad / 2, c->name, 0, 0);	
					}
					x += (1.0 / (double)n) * w;

				}
			}
		} else {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_rect(drw, x, 0, w, bh, 1, 1);
			//drw_setscheme(drw, scheme[SchemeTags]);
			// render shutdown button
			drw_text(drw, x, 0, bh, bh, lrpad / 2, "", 0, 0);
			// display help message if no application is opened
			if (!selmon->clients) {
				int titlewidth =
					TEXTW("Press space to launch an application") < selmon->btw ? TEXTW("Press space to launch an application") : (selmon->btw - bh);
				drw_text(drw, x + bh + ((selmon->btw - bh) - titlewidth + 1) / 2, 0, titlewidth, bh, 0, "Press space to launch an application", 0, 0);
			}
		}
	}
	
    // prevscheme = scheme[SchemeNorm];
	drw_setscheme(drw, scheme[SchemeNorm]);

	m->bt = n;
	m->btw = w;
	drw_map(drw, m->barwin, 0, 0, m->ww, bh);
}

void
drawbars(void)
{
	Monitor *m;

	for (m = mons; m; m = m->next)
		drawbar(m);
}

void
enternotify(XEvent *e)
{
	Client *c;
	Monitor *m;
	XCrossingEvent *ev = &e->xcrossing;

	if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != root)
		return;
	c = wintoclient(ev->window);
	m = c ? c->mon : wintomon(ev->window);
	if (m != selmon) {
		unfocus(selmon->sel, 1);
		selmon = m;
	} else if (!c || c == selmon->sel)
		return;
	focus(c);
}

void
expose(XEvent *e)
{
	Monitor *m;
	XExposeEvent *ev = &e->xexpose;

	if (ev->count == 0 && (m = wintomon(ev->window))) {
		drawbar(m);
		if (m == selmon)
			updatesystray();
	}
}

void
focus(Client *c)
{
	resetcursor();
	if (!c || !ISVISIBLE(c) || HIDDEN(c))
		for (c = selmon->stack; c && (!ISVISIBLE(c) || HIDDEN(c)); c = c->snext);
	if (selmon->sel && selmon->sel != c)
		unfocus(selmon->sel, 0);
	if (c) {
		if (c->mon != selmon)
			selmon = c->mon;
		if (c->isurgent)
			seturgent(c, 0);
		detachstack(c);
		attachstack(c);
		grabbuttons(c, 1);
		if (!c->isfloating)
			XSetWindowBorder(dpy, c->win, scheme[SchemeSel][ColBorder].pixel);
		else
			XSetWindowBorder(dpy, c->win, scheme[SchemeSel][ColFloat].pixel);

		setfocus(c);
	} else {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
	selmon->sel = c;
	if (selmon->gesture != 11 && selmon->gesture)
		selmon->gesture = 0;
	drawbars();
	if (!c){
		if (!isdesktop) {
			isdesktop = 1;
			grabkeys();
		}
	} else if (isdesktop) {
		isdesktop = 0;
		grabkeys();
	}
}

/* there are some broken focus acquiring clients needing extra handling */
void
focusin(XEvent *e)
{
	XFocusChangeEvent *ev = &e->xfocus;

	if (selmon->sel && ev->window != selmon->sel->win)
		setfocus(selmon->sel);
}

void
focusmon(const Arg *arg)
{
	Monitor *m;

	if (!mons->next)
		return;
	if ((m = dirtomon(arg->i)) == selmon)
		return;
	unfocus(selmon->sel, 0);
	selmon = m;
	focus(NULL);
}

void
focusstack(const Arg *arg)
{
	Client *c = NULL, *i;

	if (!selmon->sel)
		return;
	if (arg->i > 0) {
		for (c = selmon->sel->next; c && !ISVISIBLE(c); c = c->next);
		if (!c)
			for (c = selmon->clients; c && !ISVISIBLE(c); c = c->next);
	} else {
		for (i = selmon->clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i))
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i))
					c = i;
	}
	if (c) {
		focus(c);
		restack(selmon);
	}
}

Atom
getatomprop(Client *c, Atom prop)
{
	int di;
	unsigned long dl;
	unsigned char *p = NULL;
	Atom da, atom = None;
	/* FIXME getatomprop should return the number of items and a pointer to
	 * the stored data instead of this workaround */
	Atom req = XA_ATOM;
	if (prop == xatom[XembedInfo])
		req = xatom[XembedInfo];

	if (XGetWindowProperty(dpy, c->win, prop, 0L, sizeof atom, False, req,
		&da, &di, &dl, &dl, &p) == Success && p) {
		atom = *(Atom *)p;
		if (da == xatom[XembedInfo] && dl == 2)
			atom = ((Atom *)p)[1];
		XFree(p);
	}
	return atom;
}

int
getrootptr(int *x, int *y)
{
	int di;
	unsigned int dui;
	Window dummy;

	return XQueryPointer(dpy, root, &dummy, &dummy, x, y, &di, &di, &dui);
}

long
getstate(Window w)
{
	int format;
	long result = -1;
	unsigned char *p = NULL;
	unsigned long n, extra;
	Atom real;

	if (XGetWindowProperty(dpy, w, wmatom[WMState], 0L, 2L, False, wmatom[WMState],
		&real, &format, &n, &extra, (unsigned char **)&p) != Success)
		return -1;
	if (n != 0)
		result = *p;
	XFree(p);
	return result;
}

unsigned int
getsystraywidth()
{
	unsigned int w = 0;
	Client *i;
	if(showsystray)
		for(i = systray->icons; i; w += i->w + systrayspacing, i = i->next) ;
	return w ? w + systrayspacing : 1;
}

int
gettextprop(Window w, Atom atom, char *text, unsigned int size)
{
	char **list = NULL;
	int n;
	XTextProperty name;

	if (!text || size == 0)
		return 0;
	text[0] = '\0';
	if (!XGetTextProperty(dpy, w, &name, atom) || !name.nitems)
		return 0;
	if (name.encoding == XA_STRING)
		strncpy(text, (char *)name.value, size - 1);
	else {
		if (XmbTextPropertyToTextList(dpy, &name, &list, &n) >= Success && n > 0 && *list) {
			strncpy(text, *list, size - 1);
			XFreeStringList(list);
		}
	}
	text[size - 1] = '\0';
	XFree(name.value);
	return 1;
}

void
grabbuttons(Client *c, int focused)
{
	updatenumlockmask();
	{
		unsigned int i, j;
		unsigned int modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		if (!focused)
			XGrabButton(dpy, AnyButton, AnyModifier, c->win, False,
				BUTTONMASK, GrabModeSync, GrabModeSync, None, None);
		for (i = 0; i < LENGTH(buttons); i++)
			if (buttons[i].click == ClkClientWin)
				for (j = 0; j < LENGTH(modifiers); j++)
					XGrabButton(dpy, buttons[i].button,
						buttons[i].mask | modifiers[j],
						c->win, False, BUTTONMASK,
						GrabModeAsync, GrabModeSync, None, None);
	}
}

void
grabkeys(void)
{
	updatenumlockmask();
	{
		unsigned int i, j;
		unsigned int modifiers[] = { 0, LockMask, numlockmask, numlockmask|LockMask };
		KeyCode code;

		XUngrabKey(dpy, AnyKey, AnyModifier, root);
		for (i = 0; i < LENGTH(keys); i++) {
			if ((code = XKeysymToKeycode(dpy, keys[i].keysym)))
				for (j = 0; j < LENGTH(modifiers); j++)
					XGrabKey(dpy, code, keys[i].mod | modifiers[j], root,
						True, GrabModeAsync, GrabModeAsync);
		}
		
		if(!selmon->sel){
			for (i = 0; i < LENGTH(dkeys); i++) {
				if ((code = XKeysymToKeycode(dpy, dkeys[i].keysym)))
					for (j = 0; j < LENGTH(modifiers); j++)
						XGrabKey(dpy, code, dkeys[i].mod | modifiers[j], root,
							True, GrabModeAsync, GrabModeAsync);
			}

		}
		

	}
}

void
hide(Client *c) {
	if (!c || HIDDEN(c))
		return;

	int x, y, wi, h;
	x = c->x;
	y = c->y;
	wi = c->w;
	h = c->h;

	animateclient(c, c->x, bh - c->h + 40, 0, 0, 10, 0);

	Window w = c->win;
	static XWindowAttributes ra, ca;

	// more or less taken directly from blackbox's hide() function
	XGrabServer(dpy);
	XGetWindowAttributes(dpy, root, &ra);
	XGetWindowAttributes(dpy, w, &ca);
	// prevent UnmapNotify events
	XSelectInput(dpy, root, ra.your_event_mask & ~SubstructureNotifyMask);
	XSelectInput(dpy, w, ca.your_event_mask & ~StructureNotifyMask);
	XUnmapWindow(dpy, w);
	setclientstate(c, IconicState);
	XSelectInput(dpy, root, ra.your_event_mask);
	XSelectInput(dpy, w, ca.your_event_mask);
	XUngrabServer(dpy);
	resize(c, x, y, wi, h, 0);

	focus(c->snext);
	arrange(c->mon);
}

void
incnmaster(const Arg *arg)
{
	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag] = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

#ifdef XINERAMA
static int
isuniquegeom(XineramaScreenInfo *unique, size_t n, XineramaScreenInfo *info)
{
	while (n--)
		if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org
		&& unique[n].width == info->width && unique[n].height == info->height)
			return 0;
	return 1;
}
#endif /* XINERAMA */

void
keypress(XEvent *e)
{

	unsigned int i;
	KeySym keysym;
	XKeyEvent *ev;

	ev = &e->xkey;
	keysym = XKeycodeToKeysym(dpy, (KeyCode)ev->keycode, 0);
	for (i = 0; i < LENGTH(keys); i++) {
		if (keysym == keys[i].keysym
		&& CLEANMASK(keys[i].mod) == CLEANMASK(ev->state)
		&& keys[i].func) {
			keys[i].func(&(keys[i].arg));
		}

	}

	if (!selmon->sel) {
		for (i = 0; i < LENGTH(dkeys); i++) {
			if (keysym == dkeys[i].keysym
			&& CLEANMASK(dkeys[i].mod) == CLEANMASK(ev->state)
			&& dkeys[i].func)
				dkeys[i].func(&(dkeys[i].arg));

		}

	}

}

void
killclient(const Arg *arg)
{
	if (!selmon->sel || selmon->sel->islocked)
		return;
	animateclient(selmon->sel, selmon->sel->x, selmon->mh - 20, 0, 0, 10, 0);
	if (!sendevent(selmon->sel->win, wmatom[WMDelete], NoEventMask, wmatom[WMDelete], CurrentTime, 0 , 0, 0)) {
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSetCloseDownMode(dpy, DestroyAll);
		XKillClient(dpy, selmon->sel->win);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
}

void
manage(Window w, XWindowAttributes *wa)
{

	if (desktopicons) {
		int x, y;
		Monitor *tempmon;
		if (getrootptr(&x, &y)) {
			tempmon = recttomon(x, y, 1, 1);
			if (selmon != tempmon) {
				if (selmon->sel)
					unfocus(selmon->sel, 1);
				selmon = tempmon;
				focus(NULL);
			}
		}
	}

	Client *c, *t = NULL;
	Window trans = None;
	XWindowChanges wc;

	c = ecalloc(1, sizeof(Client));
	c->win = w;
	/* geometry */
	c->x = c->oldx = wa->x;
	c->y = c->oldy = wa->y;
	c->w = c->oldw = wa->width;
	c->h = c->oldh = wa->height;
	c->oldbw = wa->border_width;

	updatetitle(c);
	if (XGetTransientForHint(dpy, w, &trans) && (t = wintoclient(trans))) {
		c->mon = t->mon;
		c->tags = t->tags;
	} else {
		c->mon = selmon;
		applyrules(c);
	}

	if (c->x + WIDTH(c) > c->mon->mx + c->mon->mw)
		c->x = c->mon->mx + c->mon->mw - WIDTH(c);
	if (c->y + HEIGHT(c) > c->mon->my + c->mon->mh)
		c->y = c->mon->my + c->mon->mh - HEIGHT(c);
	c->x = MAX(c->x, c->mon->mx);
	/* only fix client y-offset, if the client center might cover the bar */
	c->y = MAX(c->y, ((c->mon->by == c->mon->my) && (c->x + (c->w / 2) >= c->mon->wx)
		&& (c->x + (c->w / 2) < c->mon->wx + c->mon->ww)) ? bh : c->mon->my);
	c->bw = borderpx;

	wc.border_width = c->bw;
	XConfigureWindow(dpy, w, CWBorderWidth, &wc);
	XSetWindowBorder(dpy, w, scheme[SchemeNorm][ColBorder].pixel);
	configure(c); /* propagates border_width, if size doesn't change */
	updatewindowtype(c);
	updatesizehints(c);
	updatewmhints(c);
	updatemotifhints(c);

	c->sfx = c->x;
	c->sfy = c->y;
	c->sfw = c->w;
	c->sfh = c->h;
	XSelectInput(dpy, w, EnterWindowMask|FocusChangeMask|PropertyChangeMask|StructureNotifyMask);
	grabbuttons(c, 0);
	if (!c->isfloating)
		c->isfloating = c->oldstate = trans != None || c->isfixed;
	if (c->isfloating)
		XRaiseWindow(dpy, c->win);
	attach(c);
	attachstack(c);
	XChangeProperty(dpy, root, netatom[NetClientList], XA_WINDOW, 32, PropModeAppend,
		(unsigned char *) &(c->win), 1);
	XMoveResizeWindow(dpy, c->win, c->x + 2 * sw, c->y, c->w, c->h); /* some windows require this */
	if (!HIDDEN(c))
		setclientstate(c, NormalState);
	if (c->mon == selmon)
		unfocus(selmon->sel, 0);
	c->mon->sel = c;
	arrange(c->mon);
	if (!HIDDEN(c))
		XMapWindow(dpy, c->win);
	focus(NULL);
	if (newdesktop) {
		newdesktop = 0;
		createdesktop();
	}

	if (animated) {
		resizeclient(c, c->x, c->y - 70, c->w, c->h);
		animateclient(c,c->x, c->y + 70, 0,0,7,0);
		if (c->w > selmon->mw - 30 || c->h > selmon->mh - 30)
			arrange(selmon);
	}

}

void
mappingnotify(XEvent *e)
{
	XMappingEvent *ev = &e->xmapping;

	XRefreshKeyboardMapping(ev);
	if (ev->request == MappingKeyboard)
		grabkeys();
}

void
maprequest(XEvent *e)
{
	static XWindowAttributes wa;
	XMapRequestEvent *ev = &e->xmaprequest;
	Client *i;
	if ((i = wintosystrayicon(ev->window))) {
		sendevent(i->win, netatom[Xembed], StructureNotifyMask, CurrentTime, XEMBED_WINDOW_ACTIVATE, 0, systray->win, XEMBED_EMBEDDED_VERSION);
		resizebarwin(selmon);
		updatesystray();
	}

	if (!XGetWindowAttributes(dpy, ev->window, &wa))
		return;
	if (wa.override_redirect)
		return;
	if (!wintoclient(ev->window))
		manage(ev->window, &wa);
	
}

void
monocle(Monitor *m)
{
	unsigned int n = 0;
	Client *c;
	
	if (animated && selmon->sel)
		XRaiseWindow(dpy, selmon->sel->win);

	for (c = m->clients; c; c = c->next)
		if (ISVISIBLE(c))
			n++;
	if (n > 0) /* override layout symbol */
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		animateclient(c, m->wx, m->wy, m->ww - 2 * c->bw, m->wh - 2 * c->bw, 10, 0);
}

void
motionnotify(XEvent *e)
{
	static Monitor *mon = NULL;
	Monitor *m;
	XMotionEvent *ev = &e->xmotion;

	int i;

	if (ev->window != root)
		return;

	if (ev->y_root == 0 && ev->x_root >= selmon->mx + selmon->ww +  - 20 - getsystraywidth()) {
			if (selmon->gesture != 11) {
				selmon->gesture = 11;
				setoverlay();
			}
	} else {
		if (selmon->gesture == 11 && ev->x_root >= selmon->mx + selmon->ww - 24  - getsystraywidth()) {
			selmon->gesture = 0;
		}

		// leave small deactivator zone 
		if (ev->y_root <= bh - 3) {
			if (ev->x_root < selmon->activeoffset - 50 && !selmon->showtags) {
				if (ev->x_root < selmon->mx + startmenusize) {
					selmon->gesture = 13;
					drawbar(selmon);
				} else {
					i = 0;
					int x = selmon->mx + startmenusize;
					do {
						x += TEXTW(tags[i]);
					} while (ev->x_root >= x && ++i < LENGTH(tags));
					
					if (i != selmon->gesture - 1) {
						selmon->gesture = i + 1;
						drawbar(selmon);
					}
				}
			}

			// perform gesture over layout indicator to bring up switcher
			if (ev->y_root == 0 && ev->state & ShiftMask) {
				if (ev->x_root == 0 && !topdrag) {
					spawn(&((Arg) { .v = caretinstantswitchcmd }));
					topdrag = 1;
				}
				if (!tagwidth)
					tagwidth = gettagwidth();
			} else if (topdrag) {
				topdrag = 0;
			} 

			// hover over close button
			if (selmon->sel) {
				if (ev->x_root > selmon->activeoffset && ev->x_root < (selmon->activeoffset + 32)) {
					if (selmon->gesture != 12) {
						selmon->gesture = 12;
						drawbar(selmon);
					}
				} else if (selmon->gesture == 12) {
					selmon->gesture = 0;
					drawbar(selmon);
				} else {
					// hover over resize widget
					if (!altcursor) {
						if (ev->x_root > selmon->activeoffset + (1.0 / (double)selmon->bt) * selmon->btw - 30 && ev->x_root < selmon->activeoffset + (1.0 / (double)selmon->bt) * selmon->btw) {
							XDefineCursor(dpy, root, cursor[CurResize]->cursor);
							altcursor = 1;
						}
					} else {
						if (ev->x_root < selmon->activeoffset + (1.0 / (double)selmon->bt) * selmon->btw - 30 || ev->x_root > selmon->activeoffset + (1.0 / (double)selmon->bt) * selmon->btw) {
							XDefineCursor(dpy, root, cursor[CurNormal]->cursor);
							altcursor = 0;
						}
					}
				} 
			}
			if (altcursor == 2) {
				resetcursor();
			}

		} else {
			if (selmon->gesture) {
				selmon->gesture = 0;
				drawbar(selmon);
			}

			if (ev->x_root > selmon->mx + selmon->mw - 50) {
				if (!altcursor && ev->y_root > bh + 60) {
					altcursor = 2;
					XDefineCursor(dpy, root, cursor[CurVert]->cursor);
				}
			} else if (altcursor == 2 || altcursor == 1) {
				altcursor = 0;
				XUndefineCursor(dpy, root);
				XDefineCursor(dpy, root, cursor[CurNormal]->cursor);

			}
		}
	}
	if ((m = recttomon(ev->x_root, ev->y_root, 1, 1)) != mon && mon) {
		unfocus(selmon->sel, 1);
		selmon = m;
		focus(NULL);
	}
	mon = m;
}

void
movemouse(const Arg *arg)
{
	int x, y, ocx, ocy, nx, ny, ti, tx, occ, tagclient, colorclient, tagx, notfloating;
	Client *c;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;
	tagclient = 0;
	notfloating = 0;
	if (!(c = selmon->sel))
		return;
	if (c->isfullscreen && !c->isfakefullscreen) /* no support moving fullscreen windows by mouse */
		return;

	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurMove]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&x, &y))
		return;
	bardragging = 1;
	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / (doubledraw ? 240 : 120)))
				continue;
			lasttime = ev.xmotion.time;

			nx = ocx + (ev.xmotion.x - x);
			if (ev.xmotion.y_root > bh) {
				ny = ocy + (ev.xmotion.y - y);
				if ((ev.xmotion.x_root < selmon->mx + 50 && ev.xmotion.x_root > selmon->mx - 1) || (ev.xmotion.x_root > selmon->mx + selmon->mw - 50 && ev.xmotion.x_root < selmon->mx + selmon->mw)) {
					if (!colorclient) {
						XSetWindowBorder(dpy, selmon->sel->win, scheme[SchemeAddActive][ColBg].pixel);
						colorclient = 1;
					}
				} else if (colorclient) {
					colorclient = 0;
					XSetWindowBorder(dpy, selmon->sel->win, scheme[SchemeSel][ColFloat].pixel);
				}
			} else {
				ny = bh;
				if (!colorclient) {
					colorclient = 1;
					XSetWindowBorder(dpy, selmon->sel->win, scheme[SchemeAddActive][ColBg].pixel);
				}

			}

			if (abs(selmon->wx - nx) < snap)
				nx = selmon->wx;
			else if (abs((selmon->wx + selmon->ww) - (nx + WIDTH(c))) < snap)
				nx = selmon->wx + selmon->ww - WIDTH(c);
			if (abs(selmon->wy - ny) < snap)
				ny = selmon->wy;
			else if (abs((selmon->wy + selmon->wh) - (ny + HEIGHT(c))) < snap)
				ny = selmon->wy + selmon->wh - HEIGHT(c);
			if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
			&& (abs(nx - c->x) > snap || abs(ny - c->y) > snap)) {
				if (animated) {
					animated = 0;
					togglefloating(NULL);
					animated = 1;
				} else {
					togglefloating(NULL);
				}
			}
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, nx, ny, c->w, c->h, 1);

			if (ev.xmotion.y_root < selmon->my + bh + 100) {
				if (ev.xmotion.x_root < selmon->mx || 
				ev.xmotion.x_root > selmon->mx + selmon->mw || 
				ev.xmotion.y_root < selmon->my || 
				ev.xmotion.y_root > selmon->my + selmon->mh) {
					if ((m = recttomon(ev.xmotion.x_root, ev.xmotion.y_root, 2, 2)) != selmon) {
						XRaiseWindow(dpy, c->win);
						sendmon(c, m);
						selmon = m;
						focus(NULL);
						drawbar(selmon);
					}
				}
				if (ev.xmotion.y_root < selmon->my + bh &&
					tagx != getxtag(ev.xmotion.x_root)) {
					tagx = getxtag(ev.xmotion.x_root);
					selmon->gesture = tagx + 1;
					drawbar(selmon);
				}
				
			}
			break;
		}
	} while (ev.type != ButtonRelease);

	bardragging = 0;
	if (ev.xmotion.y_root < bh) {
		if (!tagwidth)
			tagwidth = gettagwidth();

		if (ev.xmotion.x_root < selmon->mx + tagwidth && ev.xmotion.x_root > selmon->mx) {
			ti = 0;
			tx = startmenusize;
			for (c = selmon->clients; c; c = c->next)
				occ |= c->tags == 255 ? 0 : c->tags;
			do {
				// do not reserve space for vacant tags
				if (selmon->showtags){
					if (!(occ & 1 << ti || m->tagset[m->seltags] & 1 << ti))
						continue;
				}
				tx += TEXTW(tags[ti]);	
			} while (ev.xmotion.x_root >= tx + selmon->mx && ++ti < LENGTH(tags));
			selmon->sel->isfloating = 0;
			if (ev.xmotion.state & ShiftMask)
				tag(&((Arg) { .ui = 1 << ti }));
			else
				followtag(&((Arg) { .ui = 1 << ti }));
			tagclient = 1;

		} else if (ev.xmotion.x_root > selmon->mx + selmon->mw - 50 && ev.xmotion.x_root < selmon->mx + selmon->mw ) {
			resize(selmon->sel, selmon->mx + 20, bh, selmon->ww - 40, (selmon->mh) / 3, True);
			togglefloating(NULL);
			createoverlay();
			selmon->gesture = 11;
		} else if (selmon->sel->isfloating) {
			notfloating = 1;
		}
	} else {
		if (ev.xmotion.x_root > selmon->mx + selmon->mw - 50 && ev.xmotion.x_root < selmon->mx + selmon->mw  + 1) {
			// snap to half of the screen like on gnome
			if (ev.xmotion.state & ShiftMask) {
				animateclient(c, selmon->mx + (selmon->mw / 2) + 2, selmon->my + bh + 2, (selmon->mw / 2) - 8, selmon->mh - bh - 8, 15, 0);
			} else {
				if (ev.xmotion.y_root < (2 * selmon->mh) / 3)
					moveright(arg);
				else
					tagtoright(arg);
				c->isfloating = 0;
				arrange(selmon);
			}

		} else if (ev.xmotion.x_root < selmon->mx + 50 && ev.xmotion.x_root > selmon->mx - 1) {
			if (ev.xmotion.state & ShiftMask) {
				animateclient(c, selmon->mx + 2, selmon->my + bh + 2, (selmon->mw / 2) - 8, selmon->mh - bh - 8, 15, 0);
			} else {
				if (ev.xmotion.y_root < (2 * selmon->mh) / 3)
					moveleft(arg);
				else
					tagtoleft(arg);
				c->isfloating = 0;
				arrange(selmon);
			}
		}
	}	

	XUngrabPointer(dpy, CurrentTime);
	if (!tagclient && (m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
	if (notfloating)
		togglefloating(NULL);
}


void
gesturemouse(const Arg *arg)
{
	int x, y, lasty;
	Monitor *m;
	XEvent ev;
	Time lasttime = 0;
	int tmpactive = 0;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurMove]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&x, &y))
		return;
	lasty = y;
	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / (doubledraw ? 240 : 120)))
				continue;
			lasttime = ev.xmotion.time;
			if (abs(lasty - ev.xmotion.y_root) > selmon->mh / 30) {
				if (ev.xmotion.y_root < lasty)
					spawn(&((Arg) { .v = upvol }));
				else
					spawn(&((Arg) { .v = downvol }));
				lasty = ev.xmotion.y_root;
				if (!tmpactive)
					tmpactive = 1;
			}
			break;
		}
	} while (ev.type != ButtonRelease);

	if (ev.xmotion.x_root < selmon->mx + selmon->mw - 100) {
			spawn(&((Arg) { .v = onboardcmd }));
	} else {
		if (!tmpactive && abs(ev.xmotion.y_root - y) < 100) {
			spawn(&((Arg) { .v = caretinstantswitchcmd }));
		}
	}

	XUngrabPointer(dpy, CurrentTime);

}

void
dragmouse(const Arg *arg)
{
	int x, y, ocx, ocy, starty, startx, dragging, isactive, sinit;
	starty = 100;
	sinit = 0;
	dragging = 0;
	XEvent ev;
	Time lasttime = 0;

	Client *tempc = (Client*)arg->v;

	if (tempc->isfullscreen && !tempc->isfakefullscreen) /* no support moving fullscreen windows by mouse */
		return;
	if (!getrootptr(&x, &y))
		return;
	if (x > selmon->activeoffset + (1.0 / (double)selmon->bt) * selmon->btw - 30 && x < selmon->activeoffset + (1.0 / (double)selmon->bt) * selmon->btw) {
		drawwindow(NULL);
		return;
	}

	if (tempc == selmon->overlay) {
		setoverlay();
		return;
	}

	if (tempc != selmon->sel) {
		if (HIDDEN(tempc)) {
			show(tempc);
			focus(tempc);
			restack(selmon);
			return;
		}
		isactive = 0;
		focus(tempc);
		restack(selmon);
		if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurClick]->cursor, CurrentTime) != GrabSuccess)
			return;

	} else {
		if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurMove]->cursor, CurrentTime) != GrabSuccess)
			return;
		isactive = 1;
	}

	Client *c = selmon->sel;


	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;
			
			if (!sinit) {
				starty = ev.xmotion.y_root;
				startx = ev.xmotion.x_root;
				sinit = 1;
			} else {
				if ((abs((starty - ev.xmotion.y_root) * (starty - ev.xmotion.y_root)) + abs((startx - ev.xmotion.x_root) * (startx - ev.xmotion.x_root))) > 4069)
					dragging = 1;
				if (starty > 10 && ev.xmotion.y_root == 0 && c->isfloating)
					dragging = 1;
			}
		}
	} while (ev.type != ButtonRelease && !dragging);
	
	if (dragging) {
		if (!c->isfloating) {
			c->sfy = bh;
			if (animated) {
				animateclient(selmon->sel, selmon->sel->sfx, selmon->sel->sfy,
		       		selmon->sel->sfw, selmon->sel->sfh, 5, 0);
				animated = 0;
				togglefloating(NULL);
				animated = 1;
			} else {
				togglefloating(NULL);
			}
		}
			if (ev.xmotion.x_root > c->x && ev.xmotion.x_root < c->x  + c->w)
				XWarpPointer(dpy, None, root, 0, 0, 0, 0, ev.xmotion.x_root, c->y + 20);
			else
				forcewarp(c);
		movemouse(NULL);
	
	} else {
		if (isactive)
			hide(tempc);
	}

	XUngrabPointer(dpy, CurrentTime);

}


void
dragrightmouse(const Arg *arg)
{
	int x, y, starty, startx, dragging, sinit;
	starty = 100;
	sinit = 0;
	dragging = 0;
	XEvent ev;
	Time lasttime = 0;

	Client *tempc = (Client*)arg->v;

	if (tempc->isfullscreen && !tempc->isfakefullscreen) /* no support moving fullscreen windows by mouse */
		return;
	
	if (tempc == selmon->overlay) {
		focus(selmon->overlay);
		createoverlay();
	}

	Client *c = selmon->sel;

	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurResize]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&x, &y))
		return;
	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;
			
			if (!sinit) {
				starty = ev.xmotion.y_root;
				startx = ev.xmotion.x_root;
				sinit = 1;
			} else {
				if ((abs((starty - ev.xmotion.y_root) * (starty - ev.xmotion.y_root)) + abs((startx - ev.xmotion.x_root) * (startx - ev.xmotion.x_root))) > 4069)
					dragging = 1;
				if (starty > 10 && ev.xmotion.y_root == 0 && c->isfloating)
					dragging = 1;
			}
			break;
		}
	} while (ev.type != ButtonRelease && !dragging);

	if (dragging) {
		if (tempc != selmon->sel) {
			focus(tempc);
			restack(selmon);
		}
		if (tempc == selmon->overlay) {
			XWarpPointer(dpy, None, root, 0, 0, 0, 0, tempc->x + (tempc->w / 2), tempc->y + tempc->h);
		} else {
			XWarpPointer(dpy, None, root, 0, 0, 0, 0, tempc->x + tempc->w, tempc->y + tempc->h);
		}
		if (animated) {
			animated = 0;
			resizemouse(NULL);
			animated = 1;
		} else {
			resizemouse(NULL);
		}

	} else {
		if (tempc != selmon->sel) {
			focus(tempc);
		}
		zoom(NULL);
	}

	XUngrabPointer(dpy, CurrentTime);
}

void waitforclickend(const Arg *arg)
{
	XEvent ev;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
	None, cursor[CurResize]->cursor, CurrentTime) != GrabSuccess)
	return;
	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);

}


void drawwindow(const Arg *arg) {

    char str[100];
    int i;
    char strout[100];
	int dimensions[4];
    int width, height, x, y;
    char tmpstring[30] = {};
	int firstchar = 0;
    int counter = 0;
    int exitcode;
	Monitor *m;
	Client *c;

	if (!selmon->sel)
		return;
	FILE *fp = popen("instantslop", "r");

    while (fgets(str, 100, fp) != NULL) {
    	strcat(strout, str);
    }

	pclose(fp);

	if (strlen(strout) < 6) {
		return;
	}


	for (i = 0; i < strlen(strout); i++){
		if(!firstchar) {
			if (strout[i] == 'x') {
			firstchar = 1;
			}
			continue;
		}

		if (strout[i] != 'x') {
			tmpstring[strlen(tmpstring)] = strout[i];
		} else {
			dimensions[counter] = atoi(tmpstring);
			counter++;
			memset(tmpstring,0,strlen(tmpstring));
		}
	}

	x = dimensions[0];
	y = dimensions[1];
	width = dimensions[2];
	height = dimensions[3];

	if (!selmon->sel)
		return;

	c = selmon->sel;

	if (width > 50 && height > 50 && x > -40 && y > -40 && width < selmon->mw + 40 && height < selmon->mh + 40 && 
	(abs(c->w - width) > 20 || abs(c->h - height) > 20 || abs(c->x - x) > 20 || abs(c->y - y) > 20)) {
		if ((m = recttomon(x, y, width, height)) != selmon) {
			sendmon(c, m);
			selmon = m;
			focus(NULL);
		}

		if (!c->isfloating)
			togglefloating(NULL);
		animateclient(c, x, y, width - (c->bw * 2), height - (c->bw * 2), 10, 0);
		arrange(selmon);
	} else {
		fprintf(stderr, "errror %s", strout);
	}
	memset(tmpstring,0,strlen(tmpstring));

	counter = 0;
}


void
dragtag(const Arg *arg)
{
	if (!tagwidth)
		tagwidth = gettagwidth();
	if ((arg->ui & TAGMASK) != selmon->tagset[selmon->seltags]) {
		view(arg);
		return;
	}

	int x, y, i, tagx, tagi;
	int leftbar = 0;
	unsigned int occ = 0;
	Monitor *m;
	m = selmon;
	XEvent ev;
	Time lasttime = 0;
	Client *c;

	if (!selmon->sel)
		return;

	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurMove]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!getrootptr(&x, &y))
		return;
	bardragging = 1;
	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / 60))
				continue;
			lasttime = ev.xmotion.time;
			if (ev.xmotion.y_root > bh + 1)
				leftbar = 1;
		}

		if (tagx != getxtag(ev.xmotion.x_root)) {
			tagx = getxtag(ev.xmotion.x_root);
			selmon->gesture = tagx + 1;
			drawbar(selmon);
		}
		// add additional dragging code
	} while (ev.type != ButtonRelease && !leftbar);

	if (!leftbar) {
		if (ev.xmotion.x_root < selmon->mx + tagwidth) {
			if (ev.xmotion.state & ShiftMask)
				followtag(&((Arg) { .ui = 1 << getxtag(ev.xmotion.x_root) }));
			else
				tag(&((Arg) { .ui = 1 << getxtag(ev.xmotion.x_root) }));
		} else if (ev.xmotion.x_root > selmon->mx + selmon->mw - 50) {
			if (selmon->sel == selmon->overlay) {
				setoverlay();
			} else {
				createoverlay();
				selmon->gesture = 11;
			}
		}
	}
	bardragging = 0;
	XUngrabPointer(dpy, CurrentTime);

}




Client *
nexttiled(Client *c)
{
	for (; c && (c->isfloating || !ISVISIBLE(c) || HIDDEN(c)); c = c->next);
	return c;
}

void
pop(Client *c)
{
	detach(c);
	attach(c);
	focus(c);
	arrange(c->mon);
}

void
propertynotify(XEvent *e)
{
	Client *c;
	Window trans;
	XPropertyEvent *ev = &e->xproperty;

	if ((c = wintosystrayicon(ev->window))) {
		if (ev->atom == XA_WM_NORMAL_HINTS) {
			updatesizehints(c);
			updatesystrayicongeom(c, c->w, c->h);
		}
		else
			updatesystrayiconstate(c, ev);
		resizebarwin(selmon);
		updatesystray();
	}
	if ((ev->window == root) && (ev->atom == XA_WM_NAME))
		updatestatus();
	else if (ev->state == PropertyDelete)
		return; /* ignore */
	else if ((c = wintoclient(ev->window))) {
		switch(ev->atom) {
		default: break;
		case XA_WM_TRANSIENT_FOR:
			if (!c->isfloating && (XGetTransientForHint(dpy, c->win, &trans)) &&
				(c->isfloating = (wintoclient(trans)) != NULL))
				arrange(c->mon);
			break;
		case XA_WM_NORMAL_HINTS:
			updatesizehints(c);
			break;
		case XA_WM_HINTS:
			updatewmhints(c);
			drawbars();
			break;
		}
		if (ev->atom == XA_WM_NAME || ev->atom == netatom[NetWMName]) {
			updatetitle(c);
			if (c == c->mon->sel)
				drawbar(c->mon);
		}
		if (ev->atom == netatom[NetWMWindowType])
			updatewindowtype(c);
		if (ev->atom == motifatom)
			updatemotifhints(c);

	}
}

void
quit(const Arg *arg)
{
	running = 0;
}

Monitor *
recttomon(int x, int y, int w, int h)
{
	Monitor *m, *r = selmon;
	int a, area = 0;

	for (m = mons; m; m = m->next)
		if ((a = INTERSECT(x, y, w, h, m)) > area) {
			area = a;
			r = m;
		}
	return r;
}

void
removesystrayicon(Client *i)
{
	Client **ii;

	if (!showsystray || !i)
		return;
	for (ii = &systray->icons; *ii && *ii != i; ii = &(*ii)->next);
	if (ii)
		*ii = i->next;
	free(i);
}


void
resize(Client *c, int x, int y, int w, int h, int interact)
{
	if (applysizehints(c, &x, &y, &w, &h, interact))
		resizeclient(c, x, y, w, h);
}

void
resizebarwin(Monitor *m) {
	unsigned int w = m->ww;
	if (showsystray && m == systraytomon(m))
		w -= getsystraywidth();
	XMoveResizeWindow(dpy, m->barwin, m->wx, m->by, w, bh);
}

void
resizeclient(Client *c, int x, int y, int w, int h)
{
	XWindowChanges wc;

	c->oldx = c->x; c->x = wc.x = x;
	c->oldy = c->y; c->y = wc.y = y;
	c->oldw = c->w; c->w = wc.width = w;
	c->oldh = c->h; c->h = wc.height = h;
	wc.border_width = c->bw;

	if (((nexttiled(c->mon->clients) == c && !nexttiled(c->next)) &&
		(NULL != c->mon->lt[c->mon->sellt]->arrange)
	    || &monocle == c->mon->lt[c->mon->sellt]->arrange)
	    && !c->isfullscreen && !c->isfloating) {
		c->w = wc.width += c->bw * 2;
		c->h = wc.height += c->bw * 2;
		wc.border_width = 0;
	}

	XConfigureWindow(dpy, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);
	configure(c);
	XSync(dpy, False);
}

void
resizemouse(const Arg *arg)
{
	int ocx, ocy, nw, nh;
	int ocx2, ocy2, nx, ny;
	Client *c;
	Monitor *m;
	XEvent ev;
	Cursor cur;
	int horizcorner, vertcorner;
	int corner;
	int di;
	unsigned int dui;
	Window dummy;
	Time lasttime = 0;

	if (!(c = selmon->sel))
		return;
	
	if (c->isfullscreen && !c->isfakefullscreen) /* no support resizing fullscreen windows by mouse */
		return;

	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	ocx2 = c->x + c->w;
	ocy2 = c->y + c->h;
	if (!XQueryPointer (dpy, c->win, &dummy, &dummy, &di, &di, &nx, &ny, &dui))
	       return;

	if (ny > c->h / 2) { // bottom
		if (nx < c->w / 3) { //left
			if (ny < 2 * c->h / 3) {
				corner = 7; //side
				cur = cursor[CurHor]->cursor;
			} else {
				corner = 6; //corner
				cur = cursor[CurBL]->cursor;
			}
		} else if (nx > 2 * c->w / 3) { //right
			if (ny < 2 * c->h / 3) {
				corner = 3; //side
				cur = cursor[CurHor]->cursor;
			} else {
				corner = 4; //corner
				cur = cursor[CurBR]->cursor;
			}
		} else {
			//middle
			corner = 5;
			cur = cursor[CurVert]->cursor;
		}
	} else { // top
		if (nx < c->w / 3) { // left
			if (ny > c->h / 3) {
				corner = 7; //side
				cur = cursor[CurHor]->cursor;
			} else {
				corner = 0; //corner
				cur = cursor[CurTL]->cursor;
			}
		} else if (nx > 2 * c->w / 3) { //right
			if (ny > c->h / 3) {
				corner = 3; //side
				cur = cursor[CurHor]->cursor;
			} else {
				corner = 2; //corner
				cur = cursor[CurTR]->cursor;
			}
		} else {
			//cursor on middle
			corner = 1;
			cur = cursor[CurVert]->cursor;
		}
	}

	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cur, CurrentTime) != GrabSuccess)
		return;

	horizcorner = nx < c->w / 2;
	vertcorner = ny < c->h / 2;
	if (corner == 0 || corner == 2 || corner == 4 || corner == 6) {
		XWarpPointer (dpy, None, c->win, 0, 0, 0, 0,
				horizcorner ? (-c->bw) : (c->w + c->bw - 1),
				vertcorner ? (-c->bw) : (c->h + c->bw - 1));
	} else {
		if (corner == 1 || corner == 5) {
			XWarpPointer (dpy, None, c->win, 0, 0, 0, 0,
		      (c->w + c->bw - 1) / 2,
		      vertcorner ? (-c->bw) : (c->h + c->bw - 1));
		} else if (corner == 3 || corner == 7) {
			XWarpPointer (dpy, None, c->win, 0, 0, 0, 0,
		      horizcorner ? (-c->bw) : (c->w + c->bw - 1),
		      (c->h + c->bw - 1) / 2);
		}
	}

	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / (doubledraw ? 240 : 120)))
				continue;
			lasttime = ev.xmotion.time;

			if (corner != 1 && corner != 5) {
				nx = horizcorner ? ev.xmotion.x : c->x;
				nw = MAX(horizcorner ? (ocx2 - nx) : (ev.xmotion.x - ocx - 2 * c->bw + 1), 1);
			} else {
				nx = c->x;
				nw = c->w;
			}

			if (corner != 7 && corner != 3) {
				ny = vertcorner ? ev.xmotion.y : c->y;
				nh = MAX(vertcorner ? (ocy2 - ny) : (ev.xmotion.y - ocy - 2 * c->bw + 1), 1);
			} else {
				ny = c->y;
				nh = c->h;
			}

			if (c->mon->wx + nw >= selmon->wx && c->mon->wx + nw <= selmon->wx + selmon->ww
			&& c->mon->wy + nh >= selmon->wy && c->mon->wy + nh <= selmon->wy + selmon->wh)
			{
				if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
				&& (abs(nw - c->w) > snap || abs(nh - c->h) > snap)) {
					if (animated) {
						animated = 0;
						togglefloating(NULL);
						animated = 1;
					} else {
						togglefloating(NULL);
					}
				}
			}
			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating)
				resize(c, nx, ny, nw, nh, 1);
			break;
		}
	} while (ev.type != ButtonRelease);
	
	XUngrabPointer(dpy, CurrentTime);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));
	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}



void
resizeaspectmouse(const Arg *arg)
{
	int ocx, ocy, nw, nh;
	int ocx2, ocy2, nx, ny;
	Client *c;
	Monitor *m;
	XEvent ev;
	int di;
	unsigned int dui;
	Window dummy;
	Time lasttime = 0;

	if (!(c = selmon->sel))
		return;
	
	if (c->isfullscreen && !c->isfakefullscreen) /* no support resizing fullscreen windows by mouse */
		return;

	restack(selmon);
	ocx = c->x;
	ocy = c->y;
	ocx2 = c->w;
	ocy2 = c->h;
	if (XGrabPointer(dpy, root, False, MOUSEMASK, GrabModeAsync, GrabModeAsync,
		None, cursor[CurResize]->cursor, CurrentTime) != GrabSuccess)
		return;
	if (!XQueryPointer (dpy, c->win, &dummy, &dummy, &di, &di, &nx, &ny, &dui))
	       return;
	XWarpPointer (dpy, None, c->win, 0, 0, 0, 0,
		      c->w + c->bw - 1,
		      c->h + c->bw - 1);

	do {
		XMaskEvent(dpy, MOUSEMASK|ExposureMask|SubstructureRedirectMask, &ev);
		switch(ev.type) {
		case ConfigureRequest:
		case Expose:
		case MapRequest:
			handler[ev.type](&ev);
			break;
		case MotionNotify:
			if ((ev.xmotion.time - lasttime) <= (1000 / (doubledraw ? 240 : 120)))
				continue;
			lasttime = ev.xmotion.time;

			nw = MAX(ev.xmotion.x - ocx - 2 * c->bw + 1, 1);
			nh = MAX(ev.xmotion.y - ocy - 2 * c->bw + 1, 1);
			nx = c->x;
			ny = c->y;
			nw = MAX(ev.xmotion.x - ocx - 2 * c->bw + 1, 1);
			nh = MAX(ev.xmotion.y - ocy - 2 * c->bw + 1, 1);

			if (c->mon->wx + nw >= selmon->wx && c->mon->wx + nw <= selmon->wx + selmon->ww
			&& c->mon->wy + nh >= selmon->wy && c->mon->wy + nh <= selmon->wy + selmon->wh)
			{
				if (!c->isfloating && selmon->lt[selmon->sellt]->arrange
				&& (abs(nw - c->w) > snap || abs(nh - c->h) > snap))
					togglefloating(NULL);
			}

			if (!selmon->lt[selmon->sellt]->arrange || c->isfloating) {
				if (ev.xmotion.x < ocx + c->w) {
					resize(c, nx, ny, nw,  nw * (float)ocy2/ocx2, 1);
				} else if (ev.xmotion.y < ocy + c->h){
					resize(c, nx, ny, nh * (float)ocx2/ocy2, nh, 1);
				} else if (ev.xmotion.x > ocx + c->w + c->bw - 1 + 40) {
					resize(c, nx, ny, nh * (float)ocx2/ocy2, nh, 1);
				} else if (ev.xmotion.y > ocy + c->h + c->bw - 1 + 40) {
					resize(c, nx, ny, nw,  nw * (float)ocy2/ocx2, 1);
				}
			}
			break;
		}
	} while (ev.type != ButtonRelease);
	XUngrabPointer(dpy, CurrentTime);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));

	if ((m = recttomon(c->x, c->y, c->w, c->h)) != selmon) {
		sendmon(c, m);
		selmon = m;
		focus(NULL);
	}
}

void
resizerequest(XEvent *e)
{
	XResizeRequestEvent *ev = &e->xresizerequest;
	Client *i;

	if ((i = wintosystrayicon(ev->window))) {
		updatesystrayicongeom(i, ev->width, ev->height);
		resizebarwin(selmon);
		updatesystray();
	}
}

void
restack(Monitor *m)
{
	Client *c;
	XEvent ev;
	XWindowChanges wc;

	drawbar(m);
	if (!m->sel)
		return;
	if (m->sel->isfloating || !m->lt[m->sellt]->arrange)
		XRaiseWindow(dpy, m->sel->win);
	if (m->lt[m->sellt]->arrange) {
		wc.stack_mode = Below;
		wc.sibling = m->barwin;
		for (c = m->stack; c; c = c->snext)
			if (!c->isfloating && ISVISIBLE(c)) {
				XConfigureWindow(dpy, c->win, CWSibling|CWStackMode, &wc);
				wc.sibling = c->win;
			}
	}
	XSync(dpy, False);
	while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));
}

void
run(void)
{
	XEvent ev;
	/* main event loop */
	XSync(dpy, False);
	while (running && !XNextEvent(dpy, &ev))
		if (handler[ev.type])
			handler[ev.type](&ev); /* call handler */
}

void
runAutostart(void) {
	system("cd /usr/bin; ./instantautostart &");
}

void
scan(void)
{
	unsigned int i, num;
	Window d1, d2, *wins = NULL;
	XWindowAttributes wa;

	if (XQueryTree(dpy, root, &d1, &d2, &wins, &num)) {
		for (i = 0; i < num; i++) {
			if (!XGetWindowAttributes(dpy, wins[i], &wa)
			|| wa.override_redirect || XGetTransientForHint(dpy, wins[i], &d1))
				continue;
			if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
				manage(wins[i], &wa);
		}
		for (i = 0; i < num; i++) { /* now the transients */
			if (!XGetWindowAttributes(dpy, wins[i], &wa))
				continue;
			if (XGetTransientForHint(dpy, wins[i], &d1)
			&& (wa.map_state == IsViewable || getstate(wins[i]) == IconicState))
				manage(wins[i], &wa);
		}
		if (wins)
			XFree(wins);
	}
}

int gettagwidth() {
	int x, i, occ;
	i = x = 0;
	do {
		// do not reserve space for vacant tags
		if (selmon->showtags){
			if (!(occ & 1 << i || selmon->tagset[selmon->seltags] & 1 << i))
				continue;
		}
		x += TEXTW(tags[i]);
	} while (++i < LENGTH(tags));
	return x + startmenusize;
}

int getxtag(int ix) {
	int x, i, occ;
	Client *c;
	i = 0;
	x = startmenusize;
	for (c = selmon->clients; c; c = c->next)
		occ |= c->tags == 255 ? 0 : c->tags;
	
	do {
		// do not reserve space for vacant tags
		if (selmon->showtags){
			if (!(occ & 1 << i || selmon->tagset[selmon->seltags] & 1 << i))
				continue;
		}
		x += TEXTW(tags[i]);	
	} while (ix >= x + selmon->mx && ++i < LENGTH(tags));
	return i;
}

void
sendmon(Client *c, Monitor *m)
{
	if (c->mon == m)
		return;
	unfocus(c, 1);
	detach(c);
	detachstack(c);
	c->mon = m;
	c->tags = m->tagset[m->seltags]; /* assign tags of target monitor */
	attach(c);
	attachstack(c);
	focus(NULL);
	arrange(NULL);
}

void
setclientstate(Client *c, long state)
{
	long data[] = { state, None };

	XChangeProperty(dpy, c->win, wmatom[WMState], wmatom[WMState], 32,
		PropModeReplace, (unsigned char *)data, 2);
}

int
sendevent(Window w, Atom proto, int mask, long d0, long d1, long d2, long d3, long d4)
{
	int n;
	Atom *protocols, mt;
	int exists = 0;
	XEvent ev;

	if (proto == wmatom[WMTakeFocus] || proto == wmatom[WMDelete]) {
		mt = wmatom[WMProtocols];
		if (XGetWMProtocols(dpy, w, &protocols, &n)) {
			while (!exists && n--)
				exists = protocols[n] == proto;
			XFree(protocols);
		}
	}
	else {
		exists = True;
		mt = proto;
	}
	if (exists) {
		ev.type = ClientMessage;
		ev.xclient.window = w;
		ev.xclient.message_type = mt;
		ev.xclient.format = 32;
		ev.xclient.data.l[0] = d0;
		ev.xclient.data.l[1] = d1;
		ev.xclient.data.l[2] = d2;
		ev.xclient.data.l[3] = d3;
		ev.xclient.data.l[4] = d4;
		XSendEvent(dpy, w, False, mask, &ev);
	}
	return exists;
}

void
setfocus(Client *c)
{
	if (!c->neverfocus) {
		XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
		XChangeProperty(dpy, root, netatom[NetActiveWindow],
			XA_WINDOW, 32, PropModeReplace,
			(unsigned char *) &(c->win), 1);
	}
	sendevent(c->win, wmatom[WMTakeFocus], NoEventMask, wmatom[WMTakeFocus], CurrentTime, 0, 0, 0);
}

void
setfullscreen(Client *c, int fullscreen)
{
	if (fullscreen && !c->isfullscreen) {
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
			PropModeReplace, (unsigned char*)&netatom[NetWMFullscreen], 1);
		c->isfullscreen = 1;

		c->oldstate = c->isfloating;
		c->oldbw = c->bw;
		if (!c->isfakefullscreen) {
			c->bw = 0;
			if (!c->isfloating)
				animateclient(c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh, 10, 0);
			resizeclient(c, c->mon->mx, c->mon->my, c->mon->mw, c->mon->mh);
			XRaiseWindow(dpy, c->win);
		}
		c->isfloating = 1;


	} else if (!fullscreen && c->isfullscreen){
		XChangeProperty(dpy, c->win, netatom[NetWMState], XA_ATOM, 32,
			PropModeReplace, (unsigned char*)0, 0);
		c->isfullscreen = 0;

		c->isfloating = c->oldstate;
		c->bw = c->oldbw;
		c->x = c->oldx;
		c->y = c->oldy;
		c->w = c->oldw;
		c->h = c->oldh;

		if (!c->isfakefullscreen) {
			resizeclient(c, c->x, c->y, c->w, c->h);
			arrange(c->mon);
		}

	}
}

void
setlayout(const Arg *arg)
{
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag] ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt] = (Layout *)arg->v;
	strncpy(selmon->ltsymbol, selmon->lt[selmon->sellt]->symbol, sizeof selmon->ltsymbol);
	if (selmon->sel)
		arrange(selmon);
	else
		drawbar(selmon);
}

/* arg > 1.0 will set mfact absolutely */
void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
	if (f < 0.1 || f > 0.9)
		return;
	selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag] = f;
	arrange(selmon);
}

void
setup(void)
{
	int i;
	XSetWindowAttributes wa;
	Atom utf8string;

	/* clean up any zombies immediately */
	sigchld(0);

	/* init screen */
	screen = DefaultScreen(dpy);
	sw = DisplayWidth(dpy, screen);
	sh = DisplayHeight(dpy, screen);
	root = RootWindow(dpy, screen);
	drw = drw_create(dpy, screen, root, sw, sh);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;
	bh = drw->fonts->h + 12;
	updategeom();
	/* init atoms */
	utf8string = XInternAtom(dpy, "UTF8_STRING", False);
	wmatom[WMProtocols] = XInternAtom(dpy, "WM_PROTOCOLS", False);
	wmatom[WMDelete] = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
	wmatom[WMState] = XInternAtom(dpy, "WM_STATE", False);
	wmatom[WMTakeFocus] = XInternAtom(dpy, "WM_TAKE_FOCUS", False);
	netatom[NetActiveWindow] = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	netatom[NetSupported] = XInternAtom(dpy, "_NET_SUPPORTED", False);
	netatom[NetSystemTray] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
	netatom[NetSystemTrayOP] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
	netatom[NetSystemTrayOrientation] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
	netatom[NetSystemTrayOrientationHorz] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION_HORZ", False);
	netatom[NetWMName] = XInternAtom(dpy, "_NET_WM_NAME", False);
	netatom[NetWMState] = XInternAtom(dpy, "_NET_WM_STATE", False);
	netatom[NetWMCheck] = XInternAtom(dpy, "_NET_SUPPORTING_WM_CHECK", False);
	netatom[NetWMFullscreen] = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
	netatom[NetWMWindowType] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	netatom[NetWMWindowTypeDialog] = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
	netatom[NetClientList] = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
	motifatom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
	
	xatom[Manager] = XInternAtom(dpy, "MANAGER", False);
	xatom[Xembed] = XInternAtom(dpy, "_XEMBED", False);
	xatom[XembedInfo] = XInternAtom(dpy, "_XEMBED_INFO", False);
	/* init cursors */
	cursor[CurNormal] = drw_cur_create(drw, XC_left_ptr);
	cursor[CurResize] = drw_cur_create(drw, XC_crosshair);
	cursor[CurMove] = drw_cur_create(drw, XC_fleur);
	cursor[CurClick] = drw_cur_create(drw, XC_hand1);
	cursor[CurVert] = drw_cur_create(drw, XC_sb_v_double_arrow);
	cursor[CurHor] = drw_cur_create(drw, XC_sb_h_double_arrow);
	cursor[CurBL] = drw_cur_create(drw, XC_bottom_left_corner);
	cursor[CurBR] = drw_cur_create(drw, XC_bottom_right_corner);
	cursor[CurTL] = drw_cur_create(drw, XC_top_left_corner);
	cursor[CurTR] = drw_cur_create(drw, XC_top_right_corner);

	/* init appearance */

	scheme = ecalloc(LENGTH(colors) + 1, sizeof(Clr *));
	scheme[LENGTH(colors)] = drw_scm_create(drw, colors[0], 4);

	for (i = 0; i < LENGTH(colors); i++)
		scheme[i] = drw_scm_create(drw, colors[i], 4);
	/* init system tray */
	updatesystray();
	/* init bars */
	updatebars();
	updatestatus();
	/* supporting window for NetWMCheck */
	wmcheckwin = XCreateSimpleWindow(dpy, root, 0, 0, 1, 1, 0, 0, 0);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMCheck], XA_WINDOW, 32,
		PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	XChangeProperty(dpy, wmcheckwin, netatom[NetWMName], utf8string, 8,
		PropModeReplace, (unsigned char *) "dwm", 3);
	XChangeProperty(dpy, root, netatom[NetWMCheck], XA_WINDOW, 32,
		PropModeReplace, (unsigned char *) &wmcheckwin, 1);
	/* EWMH support per view */
	XChangeProperty(dpy, root, netatom[NetSupported], XA_ATOM, 32,
		PropModeReplace, (unsigned char *) netatom, NetLast);
	XDeleteProperty(dpy, root, netatom[NetClientList]);
	/* select events */
	wa.cursor = cursor[CurNormal]->cursor;
	wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask
		|ButtonPressMask|PointerMotionMask|EnterWindowMask
		|LeaveWindowMask|StructureNotifyMask|PropertyChangeMask;
	XChangeWindowAttributes(dpy, root, CWEventMask|CWCursor, &wa);
	XSelectInput(dpy, root, wa.event_mask);
	grabkeys();
	focus(NULL);
}


void
seturgent(Client *c, int urg)
{
	XWMHints *wmh;

	c->isurgent = urg;
	if (!(wmh = XGetWMHints(dpy, c->win)))
		return;
	wmh->flags = urg ? (wmh->flags | XUrgencyHint) : (wmh->flags & ~XUrgencyHint);
	XSetWMHints(dpy, c->win, wmh);
	XFree(wmh);
}

void
show(Client *c)
{
	int x, y, w, h;
	if (!c || !HIDDEN(c))
		return;

	x = c->x;
	y = c->y;
	w = c->w;
	h = c->h;

	XMapWindow(dpy, c->win);
	setclientstate(c, NormalState);
	resize(c, x, -50 , w, h, 0);
	XRaiseWindow(dpy, c->win);
	animateclient(c, x, y, 0, 0, 14, 0);
	arrange(c->mon);

}

void
showhide(Client *c)
{
	if (!c)
		return;
	if (ISVISIBLE(c)) {
		/* show clients top down */
		XMoveWindow(dpy, c->win, c->x, c->y);
		if (!c->mon->lt[c->mon->sellt]->arrange || c->isfloating && (!c->isfullscreen || c->isfakefullscreen))
			resize(c, c->x, c->y, c->w, c->h, 0);
		showhide(c->snext);
	} else {
		/* hide clients bottom up */
		showhide(c->snext);
		XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);
	}
}

void
sigchld(int unused)
{
	if (signal(SIGCHLD, sigchld) == SIG_ERR)
		die("can't install SIGCHLD handler:");
	while (0 < waitpid(-1, NULL, WNOHANG));
}

void
spawn(const Arg *arg)
{
	if (arg->v == instantmenucmd)
		instantmenumon[0] = '0' + selmon->num;
	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		fprintf(stderr, "instantwm: execvp %s", ((char **)arg->v)[0]);
		perror(" failed");
		exit(EXIT_SUCCESS);
	}
}

void
tag(const Arg *arg)
{
	if (selmon->sel && arg->ui & TAGMASK) {
		selmon->sel->tags = arg->ui & TAGMASK;
		focus(NULL);
		arrange(selmon);
	}
}

void followtag(const Arg *arg)
{
	tag(arg);
	view(arg);
}

void followview(const Arg *arg)
{
	if (!selmon->sel)
		return;
	Client *c = selmon->sel;
	view(arg);
	c->tags = selmon->tagset[selmon->seltags];
	focus(c);
	arrange(selmon);
}


void
tagmon(const Arg *arg)
{
	if (!selmon->sel || !mons->next)
		return;
	sendmon(selmon->sel, dirtomon(arg->i));
}

void
tagtoleft(const Arg *arg) {
	int oldx;
	Client *c;
	if (!selmon->sel)
		return;
	c = selmon->sel;
	oldx = c->x;
	if (!c->isfloating && animated) {
		XRaiseWindow(dpy,c->win);
		animateclient(c, c->x - (c->w / 3), c->y, 0, 0, 10, 0);
	}

	int offset = 1;
	if (arg && arg->i)
		offset=arg->i;

	if(selmon->sel != NULL
	&& __builtin_popcount(selmon->tagset[selmon->seltags] & TAGMASK) == 1
	&& selmon->tagset[selmon->seltags] > 1) {
		selmon->sel->tags >>= offset;
		focus(NULL);
		arrange(selmon);
	}
	c->x = oldx;

}

void
tagtoright(const Arg *arg) {
	int oldx;
	Client *c;
	if (!selmon->sel)
		return;
	c = selmon->sel;
	oldx = c->x;
	if (!c->isfloating && animated) {
		XRaiseWindow(dpy,c->win);
		animateclient(c, c->x + (c->w/3), c->y, 0, 0, 10, 0);
	}

	int offset = 1;
	if (arg && arg->i)
		offset=arg->i;

	if(selmon->sel != NULL
	&& __builtin_popcount(selmon->tagset[selmon->seltags] & TAGMASK) == 1
	&& selmon->tagset[selmon->seltags] & (TAGMASK >> 1)) {
		selmon->sel->tags <<= offset;
		focus(NULL);
		arrange(selmon);
	}
	c->x = oldx;

}

void
tile(Monitor *m)
{
	unsigned int i, n, h, mw, my, ty, framecount;
	Client *c;

	if (animated && clientcount() > 5)
		framecount = 4;
	else
		framecount = 7;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;

	if (n > m->nmaster)
		mw = m->nmaster ? m->ww * m->mfact : 0;
	else
		mw = m->ww;
	for (i = my = ty = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++)
		if (i < m->nmaster) {
			h = (m->wh - my) / (MIN(n, m->nmaster) - i);
			animateclient(c, m->wx, m->wy + my, mw - (2*c->bw), h - (2*c->bw), framecount, 0);
			if (my + HEIGHT(c) < m->wh)
				my += HEIGHT(c);
		} else {
			h = (m->wh - ty) / (n - i);
			animateclient(c, m->wx + mw, m->wy + ty, m->ww - mw - (2*c->bw), h - (2*c->bw), framecount, 0);
			if (ty + HEIGHT(c) < m->wh)
				ty += HEIGHT(c);
		}
}

void
togglealttag(const Arg *arg)
{
	showalttag = !showalttag;
	Monitor *m;
	for (m = mons; m; m = m->next)
		drawbar(m);
	
	tagwidth = gettagwidth();
}

void
togglesticky(const Arg *arg)
{
	if (!selmon->sel)
		return;
	selmon->sel->issticky = !selmon->sel->issticky;
	arrange(selmon);
}

void
toggleanimated(const Arg *arg)
{
	animated = !animated;
}

void
toggledoubledraw(const Arg *arg) {
	doubledraw = !doubledraw;
}

void
togglefakefullscreen(const Arg *arg) {
	if (selmon->sel->isfullscreen) {
		if (selmon->sel->isfakefullscreen) {
			resizeclient(selmon->sel, selmon->mx, selmon->my, selmon->mw, selmon->mh);
			XRaiseWindow(dpy, selmon->sel->win);
		} else {
			selmon->sel->bw = borderpx;
		}
	}
	
	selmon->sel->isfakefullscreen = !selmon->sel->isfakefullscreen;
}

void
togglelocked(const Arg *arg) {
	if (!selmon->sel)
		return;
	selmon->sel->islocked = !selmon->sel->islocked;
	drawbar(selmon);
}


void
warp(const Client *c)
{
	int x, y;

	if (!c) {
		XWarpPointer(dpy, None, root, 0, 0, 0, 0, selmon->wx + selmon->ww/2, selmon->wy + selmon->wh/2);
		return;
	}

	if (!getrootptr(&x, &y) ||
	    (x > c->x - c->bw &&
	     y > c->y - c->bw &&
	     x < c->x + c->w + c->bw*2 &&
	     y < c->y + c->h + c->bw*2) ||
	    (y > c->mon->by && y < c->mon->by + bh) ||
	    (c->mon->topbar && !y))
		return;

	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w / 2, c->h / 2);
}

void forcewarp(const Client *c){
	XWarpPointer(dpy, None, c->win, 0, 0, 0, 0, c->w / 2, 10);
}

void
warpfocus()
{
	warp(selmon->sel);
}


void
moveresize(const Arg *arg) {
	/* only floating windows can be moved */
	Client *c;
	c = selmon->sel;
	
	if (selmon->lt[selmon->sellt]->arrange && !c->isfloating)
		return;

	int mstrength = 40;
	int mpositions[4][2] = {{0, mstrength}, {0, (-1) * mstrength}, {mstrength,0}, {(-1) * mstrength,0}};
	int nx = (c->x + mpositions[arg->i][0]);
	int ny = (c->y + mpositions[arg->i][1]);
	
	if (nx < selmon->mx)
		nx = selmon->mx;
	if (ny < 0)
		ny = 0;

	if ((ny + c->h) > selmon->mh)
		ny = (selmon->mh - c->h);

	if ((nx + c->w) > (selmon->mx + selmon->mw))
		nx = ((selmon->mw + selmon->mx) - c->w);

	resize(c, nx, ny, c->w, c->h, True);
	warp(c);
}

void
keyresize(const Arg *arg) {

	if (!selmon->sel)
		return;

	Client *c;
	c = selmon->sel;

	int mstrength = 40;
	int mpositions[4][2] = {{0, mstrength}, {0, (-1) * mstrength}, {mstrength,0}, {(-1) * mstrength,0}};

	int nw = (c->w + mpositions[arg->i][0]);
	int nh = (c->h + mpositions[arg->i][1]);


	if (selmon->lt[selmon->sellt]->arrange && !c->isfloating)
	return;

	warp(c);

	resize(c, c->x, c->y, nw, nh, True);

}

void
centerwindow() {
	if (!selmon->sel)
		return;
	Client *c;
	c = selmon->sel;
	if (selmon->lt[selmon->sellt]->arrange && !c->isfloating)
		return;

	int w, h, mw, mh;
	w = c->w;
	h = c->h;
	mw = selmon->ww;
	mh = selmon->wh;
	if (w > mw || h > mh)
		return;
	if (selmon->showbar)
		resize(c, selmon->mx + (mw/2) - (w/2), selmon->my + (mh/2) - (h/2) + bh, c->w, c->h, True);
	else
		resize(c, selmon->mx + (mw/2) - (w/2), selmon->my + (mh/2) - (h/2) - bh, c->w, c->h, True);

}


void
toggleshowtags()
{
	selmon->showtags = !selmon->showtags;
	drawbar(selmon);
}

void
togglebar(const Arg *arg)
{
	selmon->showbar = selmon->pertag->showbars[selmon->pertag->curtag] = !selmon->showbar;
	updatebarpos(selmon);
	resizebarwin(selmon);
	if (showsystray) {
		XWindowChanges wc;
		if (!selmon->showbar)
			wc.y = -bh;
		else {
			wc.y = 0;
			if (!selmon->topbar)
				wc.y = selmon->mh - bh;
		}
		XConfigureWindow(dpy, systray->win, CWY, &wc);
	}
	arrange(selmon);
}

void
togglefloating(const Arg *arg)
{
	if (!selmon->sel)
		return;
	if (selmon->sel->isfullscreen && !selmon->sel->isfakefullscreen) /* no support for fullscreen windows */
		return;
	selmon->sel->isfloating = !selmon->sel->isfloating || selmon->sel->isfixed;
	if (selmon->sel->isfloating) {
		/* restore last known float dimensions */
		XSetWindowBorder(dpy, selmon->sel->win, scheme[SchemeSel][ColFloat].pixel);
		animateclient(selmon->sel, selmon->sel->sfx, selmon->sel->sfy,
		       selmon->sel->sfw, selmon->sel->sfh, 7, 0);
	}
	else {
		XSetWindowBorder(dpy, selmon->sel->win, scheme[SchemeSel][ColBorder].pixel);
		/* save last known float dimensions */
		selmon->sel->sfx = selmon->sel->x;
		selmon->sel->sfy = selmon->sel->y;
		selmon->sel->sfw = selmon->sel->w;
		selmon->sel->sfh = selmon->sel->h;
	}
	arrange(selmon);
}

void
changefloating(Client *c)
{
	if (!c)
		return;
	if (c->isfullscreen && !c->isfakefullscreen) /* no support for fullscreen windows */
		return;
	c->isfloating = !c->isfloating || c->isfixed;
	if (c->isfloating)
		/* restore last known float dimensions */
		resize(c, c->sfx, c->sfy,
		       c->sfw, c->sfh, False);
	else {
		/* save last known float dimensions */
		c->sfx = c->x;
		c->sfy = c->y;
		c->sfw = c->w;
		c->sfh = c->h;
	}
	arrange(selmon);
}

void
toggletag(const Arg *arg)
{
	unsigned int newtags;

	if (!selmon->sel)
		return;
	newtags = selmon->sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		selmon->sel->tags = newtags;
		focus(NULL);
		arrange(selmon);
	}
}

void
toggleview(const Arg *arg)
{
	unsigned int newtagset = selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK);
	int i;

	if (newtagset) {
		selmon->tagset[selmon->seltags] = newtagset;

		if (newtagset == ~0) {
			selmon->pertag->prevtag = selmon->pertag->curtag;
			selmon->pertag->curtag = 0;
		}

		/* test if the user did not select the same tag */
		if (!(newtagset & 1 << (selmon->pertag->curtag - 1))) {
			selmon->pertag->prevtag = selmon->pertag->curtag;
			for (i = 0; !(newtagset & 1 << i); i++) ;
			selmon->pertag->curtag = i + 1;
		}

		/* apply settings for this view */
		selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
		selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
		selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
		selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt];
		selmon->lt[selmon->sellt^1] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt^1];

		if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
			togglebar(NULL);

		focus(NULL);
		arrange(selmon);
	}
}

void hidewin(const Arg *arg)
{
	if (!selmon->sel)
		return;
	Client *c = selmon->sel;
	if (HIDDEN(c))
		return;
	hide(c);
}

void
unhideall(const Arg *arg) {

	Client *c;
	for (c = selmon->clients; c; c = c->next) {
		if (ISVISIBLE(c) && HIDDEN(c))
			show(c);
	}
	focus(c);
	restack(selmon);

}

void
closewin(const Arg *arg)
{
	Client *c = (Client*)arg->v;

	if (!c || c->islocked)
		return;
	if (!sendevent(c->win, wmatom[WMDelete], NoEventMask, wmatom[WMDelete], CurrentTime, 0 , 0, 0)) {
		XGrabServer(dpy);
		XSetErrorHandler(xerrordummy);
		XSetCloseDownMode(dpy, DestroyAll);
		XKillClient(dpy, c->win);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}

}


void
unfocus(Client *c, int setfocus)
{
	if (!c)
		return;
	grabbuttons(c, 0);
	XSetWindowBorder(dpy, c->win, scheme[SchemeNorm][ColBorder].pixel);
	if (setfocus) {
		XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
		XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
	}
}

void
unmanage(Client *c, int destroyed)
{
	Monitor *m = c->mon;
	XWindowChanges wc;

	detach(c);
	detachstack(c);
	if (!destroyed) {
		wc.border_width = c->oldbw;
		XGrabServer(dpy); /* avoid race conditions */
		XSetErrorHandler(xerrordummy);
		XConfigureWindow(dpy, c->win, CWBorderWidth, &wc); /* restore border */
		XUngrabButton(dpy, AnyButton, AnyModifier, c->win);
		setclientstate(c, WithdrawnState);
		XSync(dpy, False);
		XSetErrorHandler(xerror);
		XUngrabServer(dpy);
	}
	free(c);
	focus(NULL);
	updateclientlist();
	arrange(m);
}

void
unmapnotify(XEvent *e)
{
	Client *c;
	XUnmapEvent *ev = &e->xunmap;

	if ((c = wintoclient(ev->window))) {
		if (ev->send_event)
			setclientstate(c, WithdrawnState);
		else
			unmanage(c, 0);
	}
	else if ((c = wintosystrayicon(ev->window))) {
		/* KLUDGE! sometimes icons occasionally unmap their windows, but do
		 * _not_ destroy them. We map those windows back */
		XMapRaised(dpy, c->win);
		updatesystray();
	}
}

void
updatebars(void)
{
	unsigned int w;
	Monitor *m;
	XSetWindowAttributes wa = {
		.override_redirect = True,
		.background_pixmap = ParentRelative,
		.event_mask = ButtonPressMask|ExposureMask
	};
	XClassHint ch = {"dwm", "dwm"};
	for (m = mons; m; m = m->next) {
		if (m->barwin)
			continue;
		w = m->ww;
		if (showsystray && m == systraytomon(m))
			w -= getsystraywidth();
		m->barwin = XCreateWindow(dpy, root, m->wx, m->by, w, bh, 0, DefaultDepth(dpy, screen),
				CopyFromParent, DefaultVisual(dpy, screen),
				CWOverrideRedirect|CWBackPixmap|CWEventMask, &wa);
		//XDefineCursor(dpy, m->barwin, cursor[CurNormal]->cursor);
		if (showsystray && m == systraytomon(m))
			XMapRaised(dpy, systray->win);
		XMapRaised(dpy, m->barwin);
		XSetClassHint(dpy, m->barwin, &ch);
	}
}

void
updatebarpos(Monitor *m)
{
	m->wy = m->my;
	m->wh = m->mh;
	if (m->showbar) {
		m->wh -= bh;
		m->by = m->topbar ? m->wy : m->wy + m->wh;
		m->wy = m->topbar ? m->wy + bh : m->wy;
	} else
		m->by = -bh;
}

void
updateclientlist()
{
	Client *c;
	Monitor *m;

	XDeleteProperty(dpy, root, netatom[NetClientList]);
	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			XChangeProperty(dpy, root, netatom[NetClientList],
				XA_WINDOW, 32, PropModeAppend,
				(unsigned char *) &(c->win), 1);
}

int
updategeom(void)
{
	int dirty = 0;

#ifdef XINERAMA
	if (XineramaIsActive(dpy)) {
		int i, j, n, nn;
		Client *c;
		Monitor *m;
		XineramaScreenInfo *info = XineramaQueryScreens(dpy, &nn);
		XineramaScreenInfo *unique = NULL;

		for (n = 0, m = mons; m; m = m->next, n++);
		/* only consider unique geometries as separate screens */
		unique = ecalloc(nn, sizeof(XineramaScreenInfo));
		for (i = 0, j = 0; i < nn; i++)
			if (isuniquegeom(unique, j, &info[i]))
				memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
		XFree(info);
		nn = j;
		if (n <= nn) { /* new monitors available */
			for (i = 0; i < (nn - n); i++) {
				for (m = mons; m && m->next; m = m->next);
				if (m)
					m->next = createmon();
				else
					mons = createmon();
			}
			for (i = 0, m = mons; i < nn && m; m = m->next, i++)
				if (i >= n
				|| unique[i].x_org != m->mx || unique[i].y_org != m->my
				|| unique[i].width != m->mw || unique[i].height != m->mh)
				{
					dirty = 1;
					m->num = i;
					m->mx = m->wx = unique[i].x_org;
					m->my = m->wy = unique[i].y_org;
					m->mw = m->ww = unique[i].width;
					m->mh = m->wh = unique[i].height;
					updatebarpos(m);
				}
		} else { /* less monitors available nn < n */
			for (i = nn; i < n; i++) {
				for (m = mons; m && m->next; m = m->next);
				while ((c = m->clients)) {
					dirty = 1;
					m->clients = c->next;
					detachstack(c);
					c->mon = mons;
					attach(c);
					attachstack(c);
				}
				if (m == selmon)
					selmon = mons;
				cleanupmon(m);
			}
		}
		free(unique);
	} else
#endif /* XINERAMA */
	{ /* default monitor setup */
		if (!mons)
			mons = createmon();
		if (mons->mw != sw || mons->mh != sh) {
			dirty = 1;
			mons->mw = mons->ww = sw;
			mons->mh = mons->wh = sh;
			updatebarpos(mons);
		}
	}
	if (dirty) {
		selmon = mons;
		selmon = wintomon(root);
	}
	return dirty;
}


// fix issues with custom window borders
void
updatemotifhints(Client *c)
{
	Atom real;
	int format;
	unsigned char *p = NULL;
	unsigned long n, extra;
	unsigned long *motif;
	int width, height;

	if (!decorhints)
		return;

	if (XGetWindowProperty(dpy, c->win, motifatom, 0L, 5L, False, motifatom,
	                       &real, &format, &n, &extra, &p) == Success && p != NULL) {
		motif = (unsigned long*)p;
		if (motif[MWM_HINTS_FLAGS_FIELD] & MWM_HINTS_DECORATIONS) {
			width = WIDTH(c);
			height = HEIGHT(c);

			if (motif[MWM_HINTS_DECORATIONS_FIELD] & MWM_DECOR_ALL ||
			    motif[MWM_HINTS_DECORATIONS_FIELD] & MWM_DECOR_BORDER ||
			    motif[MWM_HINTS_DECORATIONS_FIELD] & MWM_DECOR_TITLE)
				c->bw = c->oldbw = borderpx;
			else
				c->bw = c->oldbw = 0;

			resize(c, c->x, c->y, width - (2*c->bw), height - (2*c->bw), 0);
		}
		XFree(p);
	}
}



void
updatenumlockmask(void)
{
	unsigned int i, j;
	XModifierKeymap *modmap;

	numlockmask = 0;
	modmap = XGetModifierMapping(dpy);
	for (i = 0; i < 8; i++)
		for (j = 0; j < modmap->max_keypermod; j++)
			if (modmap->modifiermap[i * modmap->max_keypermod + j]
				== XKeysymToKeycode(dpy, XK_Num_Lock))
				numlockmask = (1 << i);
	XFreeModifiermap(modmap);
}

void
updatesizehints(Client *c)
{
	long msize;
	XSizeHints size;

	if (!XGetWMNormalHints(dpy, c->win, &size, &msize))
		/* size is uninitialized, ensure that size.flags aren't used */
		size.flags = PSize;
	if (size.flags & PBaseSize) {
		c->basew = size.base_width;
		c->baseh = size.base_height;
	} else if (size.flags & PMinSize) {
		c->basew = size.min_width;
		c->baseh = size.min_height;
	} else
		c->basew = c->baseh = 0;
	if (size.flags & PResizeInc) {
		c->incw = size.width_inc;
		c->inch = size.height_inc;
	} else
		c->incw = c->inch = 0;
	if (size.flags & PMaxSize) {
		c->maxw = size.max_width;
		c->maxh = size.max_height;
	} else
		c->maxw = c->maxh = 0;
	if (size.flags & PMinSize) {
		c->minw = size.min_width;
		c->minh = size.min_height;
	} else if (size.flags & PBaseSize) {
		c->minw = size.base_width;
		c->minh = size.base_height;
	} else
		c->minw = c->minh = 0;
	if (size.flags & PAspect) {
		c->mina = (float)size.min_aspect.y / size.min_aspect.x;
		c->maxa = (float)size.max_aspect.x / size.max_aspect.y;
	} else
		c->maxa = c->mina = 0.0;
	c->isfixed = (c->maxw && c->maxh && c->maxw == c->minw && c->maxh == c->minh);
}

void
updatestatus(void)
{
	if (!gettextprop(root, XA_WM_NAME, stext, sizeof(stext)))
		strcpy(stext, "instantwm-"VERSION);
	drawbar(selmon);
	updatesystray();
}

void
updatesystrayicongeom(Client *i, int w, int h)
{
	if (i) {
		i->h = bh;
		if (w == h)
			i->w = bh;
		else if (h == bh)
			i->w = w;
		else
			i->w = (int) ((float)bh * ((float)w / (float)h));
		applysizehints(i, &(i->x), &(i->y), &(i->w), &(i->h), False);
		/* force icons into the systray dimenons if they don't want to */
		if (i->h > bh) {
			if (i->w == i->h)
				i->w = bh;
			else
				i->w = (int) ((float)bh * ((float)i->w / (float)i->h));
			i->h = bh;
		}
	}
}

void
updatesystrayiconstate(Client *i, XPropertyEvent *ev)
{
	long flags;
	int code = 0;

	if (!showsystray || !i || ev->atom != xatom[XembedInfo] ||
			!(flags = getatomprop(i, xatom[XembedInfo])))
		return;

	if (flags & XEMBED_MAPPED && !i->tags) {
		i->tags = 1;
		code = XEMBED_WINDOW_ACTIVATE;
		XMapRaised(dpy, i->win);
		setclientstate(i, NormalState);
	}
	else if (!(flags & XEMBED_MAPPED) && i->tags) {
		i->tags = 0;
		code = XEMBED_WINDOW_DEACTIVATE;
		XUnmapWindow(dpy, i->win);
		setclientstate(i, WithdrawnState);
	}
	else
		return;
	sendevent(i->win, xatom[Xembed], StructureNotifyMask, CurrentTime, code, 0,
			systray->win, XEMBED_EMBEDDED_VERSION);
}

void
updatesystray(void)
{
	XSetWindowAttributes wa;
	XWindowChanges wc;
	Client *i;
	Monitor *m = systraytomon(NULL);
	unsigned int x = m->mx + m->mw;
	unsigned int w = 1;

	if (!showsystray)
		return;
	if (!systray) {
		/* init systray */
		if (!(systray = (Systray *)calloc(1, sizeof(Systray))))
			die("fatal: could not malloc() %u bytes\n", sizeof(Systray));
		systray->win = XCreateSimpleWindow(dpy, root, x, m->by, w, bh, 0, 0, scheme[SchemeSel][ColBg].pixel);
		wa.event_mask        = ButtonPressMask | ExposureMask;
		wa.override_redirect = True;
		wa.background_pixel  = scheme[SchemeNorm][ColBg].pixel;
		XSelectInput(dpy, systray->win, SubstructureNotifyMask);
		XChangeProperty(dpy, systray->win, netatom[NetSystemTrayOrientation], XA_CARDINAL, 32,
				PropModeReplace, (unsigned char *)&netatom[NetSystemTrayOrientationHorz], 1);
		XChangeWindowAttributes(dpy, systray->win, CWEventMask|CWOverrideRedirect|CWBackPixel, &wa);
		XMapRaised(dpy, systray->win);
		XSetSelectionOwner(dpy, netatom[NetSystemTray], systray->win, CurrentTime);
		if (XGetSelectionOwner(dpy, netatom[NetSystemTray]) == systray->win) {
			sendevent(root, xatom[Manager], StructureNotifyMask, CurrentTime, netatom[NetSystemTray], systray->win, 0, 0);
			XSync(dpy, False);
		}
		else {
			fprintf(stderr, "instantwm: unable to obtain system tray.\n");
			free(systray);
			systray = NULL;
			return;
		}
	}
	for (w = 0, i = systray->icons; i; i = i->next) {
		/* make sure the background color stays the same */
		wa.background_pixel  = scheme[SchemeNorm][ColBg].pixel;
		XChangeWindowAttributes(dpy, i->win, CWBackPixel, &wa);
		XMapRaised(dpy, i->win);
		w += systrayspacing;
		i->x = w;
		XMoveResizeWindow(dpy, i->win, i->x, 0, i->w, i->h);
		w += i->w;
		if (i->mon != m)
			i->mon = m;
	}
	w = w ? w + systrayspacing : 1;
	x -= w;
	XMoveResizeWindow(dpy, systray->win, x, m->by, w, bh);
	wc.x = x; wc.y = m->by; wc.width = w; wc.height = bh;
	wc.stack_mode = Above; wc.sibling = m->barwin;
	XConfigureWindow(dpy, systray->win, CWX|CWY|CWWidth|CWHeight|CWSibling|CWStackMode, &wc);
	XMapWindow(dpy, systray->win);
	XMapSubwindows(dpy, systray->win);
	/* redraw background */
	XSetForeground(dpy, drw->gc, scheme[SchemeNorm][ColBg].pixel);
	XFillRectangle(dpy, systray->win, drw->gc, 0, 0, w, bh);
	XSync(dpy, False);
}

void
updatetitle(Client *c)
{
	if (!gettextprop(c->win, netatom[NetWMName], c->name, sizeof c->name))
		gettextprop(c->win, XA_WM_NAME, c->name, sizeof c->name);
	if (c->name[0] == '\0') /* hack to mark broken clients */
		strcpy(c->name, broken);
}

void
updatewindowtype(Client *c)
{
	Atom state = getatomprop(c, netatom[NetWMState]);
	Atom wtype = getatomprop(c, netatom[NetWMWindowType]);

	if (state == netatom[NetWMFullscreen])
		setfullscreen(c, 1);
	if (wtype == netatom[NetWMWindowTypeDialog])
		c->isfloating = 1;
}

void
updatewmhints(Client *c)
{
	XWMHints *wmh;

	if ((wmh = XGetWMHints(dpy, c->win))) {
		if (c == selmon->sel && wmh->flags & XUrgencyHint) {
			wmh->flags &= ~XUrgencyHint;
			XSetWMHints(dpy, c->win, wmh);
		} else
			c->isurgent = (wmh->flags & XUrgencyHint) ? 1 : 0;
		if (wmh->flags & InputHint)
			c->neverfocus = !wmh->input;
		else
			c->neverfocus = 0;
		XFree(wmh);
	}
}

void
view(const Arg *arg)
{
	int i;

	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK) {
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
		selmon->pertag->prevtag = selmon->pertag->curtag;

		if (arg->ui == ~0)
			selmon->pertag->curtag = 0;
		else {
			for (i = 0; !(arg->ui & 1 << i); i++) ;
			selmon->pertag->curtag = i + 1;
		}
	} else {
		unsigned int tmptag;
		tmptag = selmon->pertag->prevtag;
		selmon->pertag->prevtag = selmon->pertag->curtag;
		selmon->pertag->curtag = tmptag;
	}

	selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
	selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
	selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
	selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt];
	selmon->lt[selmon->sellt^1] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt^1];

	if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
		togglebar(NULL);

	focus(NULL);
	arrange(selmon);
}

void
moveleft(const Arg *arg) {
	tagtoleft(arg);
	viewtoleft(arg);	
}

animleft(const Arg *arg) {
	Client *c;
	if (!selmon->sel || clientcount() != 1) {
		viewtoleft(arg);
		return;
	}
	c = selmon->sel;

	animateclient(c, c->x + 50, c->y, 0,0,10,1);
	viewtoleft(arg);
}


animright(const Arg *arg) {
	Client *c;
	if (!selmon->sel || clientcount() != 1) {
		viewtoright(arg);
		return;
	}
	c = selmon->sel;

	animateclient(c, c->x - 50, c->y, 0,0,10,1);
	viewtoright(arg);
}



void
viewtoleft(const Arg *arg) {
	int i;

	if(__builtin_popcount(selmon->tagset[selmon->seltags] & TAGMASK) == 1
	&& selmon->tagset[selmon->seltags] > 1) {
		selmon->seltags ^= 1; /* toggle sel tagset */
		selmon->tagset[selmon->seltags] = selmon->tagset[selmon->seltags ^ 1] >> 1;
		selmon->pertag->prevtag = selmon->pertag->curtag;

		if (selmon->tagset[selmon->seltags ^ 1] >> 1 == ~0)
			selmon->pertag->curtag = 0;
		else {
			for (i = 0; !(selmon->tagset[selmon->seltags ^ 1] >> 1 & 1 << i); i++) ;
			selmon->pertag->curtag = i + 1;
		}

		selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
		selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
		selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
		selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt];
		selmon->lt[selmon->sellt^1] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt^1];

		if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
			togglebar(NULL);

		focus(NULL);
		arrange(selmon);
	}
}



void
shiftview(const Arg *arg)
{
	Arg a;
	Client *c;
	unsigned visible = 0;
	int i = arg->i;
	int count = 0;
	int nextseltags, curseltags = selmon->tagset[selmon->seltags];

	do {
		if(i > 0) // left circular shift
			nextseltags = (curseltags << i) | (curseltags >> (LENGTH(tags) - i));

		else // right circular shift
			nextseltags = curseltags >> (- i) | (curseltags << (LENGTH(tags) + i));

                // Check if tag is visible
		for (c = selmon->clients; c && !visible; c = c->next)
			if (nextseltags & c->tags) {
				visible = 1;
				break;
			}
		i += arg->i;
	} while (!visible && ++count < 10);

	if (count < 10) {
		a.i = nextseltags;
		view(&a);
	}
}



void
viewtoright(const Arg *arg) {
	int i;

	if(__builtin_popcount(selmon->tagset[selmon->seltags] & TAGMASK) == 1
	&& selmon->tagset[selmon->seltags] & (TAGMASK >> 1)) {
		selmon->seltags ^= 1; /* toggle sel tagset */
		selmon->tagset[selmon->seltags] = selmon->tagset[selmon->seltags ^ 1] << 1;
		
		selmon->pertag->prevtag = selmon->pertag->curtag;

		if (selmon->tagset[selmon->seltags ^ 1] << 1 == ~0)
			selmon->pertag->curtag = 0;
		else {
			for (i = 0; !(selmon->tagset[selmon->seltags ^ 1] << 1 & 1 << i); i++) ;
			selmon->pertag->curtag = i + 1;
		}

		selmon->nmaster = selmon->pertag->nmasters[selmon->pertag->curtag];
		selmon->mfact = selmon->pertag->mfacts[selmon->pertag->curtag];
		selmon->sellt = selmon->pertag->sellts[selmon->pertag->curtag];
		selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt];
		selmon->lt[selmon->sellt^1] = selmon->pertag->ltidxs[selmon->pertag->curtag][selmon->sellt^1];

		if (selmon->showbar != selmon->pertag->showbars[selmon->pertag->curtag])
			togglebar(NULL);

		
		focus(NULL);
		arrange(selmon);
	}
}


void
moveright(const Arg *arg) {
	tagtoright(arg);
	viewtoright(arg);	
}

// toggle overview like layout
void
overtoggle(const Arg *arg){

	if (!selmon->pertag->curtag == 0) {
		selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[0][selmon->sellt] = (Layout *)&layouts[6];
		view(arg);
		if (selmon->lt[selmon->sellt] != (Layout *)&layouts[6] )
			setlayout(&((Arg) { .v = &layouts[6] }));
	} else {
		winview(NULL);
	}
}

// overtoggle but with monocle layout
void
fullovertoggle(const Arg *arg){
	if (!selmon->pertag->curtag == 0) {
		selmon->lt[selmon->sellt] = selmon->pertag->ltidxs[0][selmon->sellt] = (Layout *)&layouts[3];
		view(arg);
	} else {
		winview(NULL);
	}
}

static void
bstack(Monitor *m) {
	int w, h, mh, mx, tx, ty, tw;
	unsigned int i, n;
	Client *c;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;
	if (n > m->nmaster) {
		mh = m->nmaster ? m->mfact * m->wh : 0;
		tw = m->ww / (n - m->nmaster);
		ty = m->wy + mh;
	} else {
		mh = m->wh;
		tw = m->ww;
		ty = m->wy;
	}
	for (i = mx = 0, tx = m->wx, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++) {
		if (i < m->nmaster) {
			w = (m->ww - mx) / (MIN(n, m->nmaster) - i);
			animateclient(c, m->wx + mx, m->wy, w - (2 * c->bw), mh - (2 * c->bw), 10, 0);
			mx += WIDTH(c);
		} else {
			h = m->wh - mh;
			animateclient(c, tx, ty, tw - (2 * c->bw), h - (2 * c->bw), 10, 0);
			if (tw != m->ww)
				tx += WIDTH(c);
		}
	}
}

static void
bstackhoriz(Monitor *m) {
	int w, mh, mx, tx, ty, th;
	unsigned int i, n;
	Client *c;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	if (n == 0)
		return;
	if (n > m->nmaster) {
		mh = m->nmaster ? m->mfact * m->wh : 0;
		th = (m->wh - mh) / (n - m->nmaster);
		ty = m->wy + mh;
	} else {
		th = mh = m->wh;
		ty = m->wy;
	}
	for (i = mx = 0, tx = m->wx, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++) {
		if (i < m->nmaster) {
			w = (m->ww - mx) / (MIN(n, m->nmaster) - i);
			animateclient(c, m->wx + mx, m->wy, w - (2 * c->bw), mh - (2 * c->bw), 10, 0);
			mx += WIDTH(c);
		} else {
		animateclient(c, tx, ty, m->ww - (2 * c->bw), th - (2 * c->bw), 10, 0);
			if (th != m->wh)
				ty += HEIGHT(c);
		}
	}
}



Client *
wintoclient(Window w)
{
	Client *c;
	Monitor *m;

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			if (c->win == w)
				return c;
	return NULL;
}

Client *
wintosystrayicon(Window w) {
	Client *i = NULL;

	if (!showsystray || !w)
		return i;
	for (i = systray->icons; i && i->win != w; i = i->next) ;
	return i;
}

Monitor *
wintomon(Window w)
{
	int x, y;
	Client *c;
	Monitor *m;

	if (w == root && getrootptr(&x, &y))
		return recttomon(x, y, 1, 1);
	for (m = mons; m; m = m->next)
		if (w == m->barwin)
			return m;
	if ((c = wintoclient(w)))
		return c->mon;
	return selmon;
}

/* Selects for the view of the focused window. The list of tags */
/* to be displayed is matched to the focused window tag list. */
void
winview(const Arg* arg){
	Window win, win_r, win_p, *win_c;
	unsigned nc;
	int unused;
	Client* c;
	Arg a;

	if (!XGetInputFocus(dpy, &win, &unused)) return;
	while(XQueryTree(dpy, win, &win_r, &win_p, &win_c, &nc)
	      && win_p != win_r) win = win_p;

	if (!(c = wintoclient(win))) return;

	a.ui = c->tags;
	view(&a);
}

/* There's no way to check accesses to destroyed windows, thus those cases are
 * ignored (especially on UnmapNotify's). Other types of errors call Xlibs
 * default error handler, which may call exit. */
int
xerror(Display *dpy, XErrorEvent *ee)
{
	if (ee->error_code == BadWindow
	|| (ee->request_code == X_SetInputFocus && ee->error_code == BadMatch)
	|| (ee->request_code == X_PolyText8 && ee->error_code == BadDrawable)
	|| (ee->request_code == X_PolyFillRectangle && ee->error_code == BadDrawable)
	|| (ee->request_code == X_PolySegment && ee->error_code == BadDrawable)
	|| (ee->request_code == X_ConfigureWindow && ee->error_code == BadMatch)
	|| (ee->request_code == X_GrabButton && ee->error_code == BadAccess)
	|| (ee->request_code == X_GrabKey && ee->error_code == BadAccess)
	|| (ee->request_code == X_CopyArea && ee->error_code == BadDrawable))
		return 0;
	fprintf(stderr, "instantwm: fatal error: request code=%d, error code=%d\n",
		ee->request_code, ee->error_code);
	return xerrorxlib(dpy, ee); /* may call exit */
}

int
xerrordummy(Display *dpy, XErrorEvent *ee)
{
	return 0;
}

/* Startup Error handler to check if another window manager
 * is already running. */
int
xerrorstart(Display *dpy, XErrorEvent *ee)
{
	die("instantwm: another window manager is already running");
	return -1;
}

Monitor *
systraytomon(Monitor *m) {
	Monitor *t;
	int i, n;
	if(!systraypinning) {
		if(!m)
			return selmon;
		return m == selmon ? m : NULL;
	}
	for(n = 1, t = mons; t && t->next; n++, t = t->next) ;
	for(i = 1, t = mons; t && t->next && i < systraypinning; i++, t = t->next) ;
	if(systraypinningfailfirst && n < systraypinning)
		return mons;
	return t;
}

void
zoom(const Arg *arg)
{
	Client *c = selmon->sel;
	XRaiseWindow(dpy, c->win);
	if (!selmon->lt[selmon->sellt]->arrange
	|| (selmon->sel && selmon->sel->isfloating))
		return;
	if (c == nexttiled(selmon->clients))
		if (!c || !(c = nexttiled(c->next)))
			return;
	pop(c);
}

int
main(int argc, char *argv[])
{
	if (argc == 2 && !strcmp("-v", argv[1]))
		die("instantwm-"VERSION);
	else if (argc != 1)
		die("usage: instantwm [-v]");
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("instantwm: cannot open display");
	checkotherwm();
	setup();
#ifdef __OpenBSD__
	if (pledge("stdio rpath proc exec", NULL) == -1)
		die("pledge");
#endif /* __OpenBSD__ */
	scan();
	runAutostart();
	run();
	cleanup();
	XCloseDisplay(dpy);
	return EXIT_SUCCESS;
}
