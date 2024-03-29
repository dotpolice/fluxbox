// fluxbox.cc for Fluxbox Window Manager
// Copyright (c) 2001 - 2006 Henrik Kinnunen (fluxgen at fluxbox dot org)
//
// blackbox.cc for blackbox - an X11 Window manager
// Copyright (c) 1997 - 2000 Brad Hughes (bhughes at tcac.net)
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

#include "fluxbox.hh"

#include "Screen.hh"
#include "Window.hh"
#include "Workspace.hh"
#include "AtomHandler.hh"
#include "FbCommands.hh"
#include "WinClient.hh"
#include "Keys.hh"
#include "FbAtoms.hh"
#include "FocusControl.hh"
#include "Layer.hh"

#include "defaults.hh"
#include "Debug.hh"

#include "FbTk/I18n.hh"
#include "FbTk/Image.hh"
#include "FbTk/FileUtil.hh"
#include "FbTk/ImageControl.hh"
#include "FbTk/EventManager.hh"
#include "FbTk/StringUtil.hh"
#include "FbTk/Util.hh"
#include "FbTk/Resource.hh"
#include "FbTk/SimpleCommand.hh"
#include "FbTk/XrmDatabaseHelper.hh"
#include "FbTk/Command.hh"
#include "FbTk/RefCount.hh"
#include "FbTk/CompareEqual.hh"
#include "FbTk/Transparent.hh"
#include "FbTk/Select2nd.hh"
#include "FbTk/Compose.hh"
#include "FbTk/KeyUtil.hh"
#include "FbTk/MemFun.hh"

//Use GNU extensions
#ifndef	 _GNU_SOURCE
#define	 _GNU_SOURCE
#endif // _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif // HAVE_CONFIG_H

#ifdef USE_NEWWMSPEC
#include "Ewmh.hh"
#endif // USE_NEWWMSPEC
#ifdef REMEMBER
#include "Remember.hh"
#endif // REMEMBER

// X headers
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

// X extensions
#ifdef SHAPE
#include <X11/extensions/shape.h>
#endif // SHAPE
#ifdef HAVE_RANDR
#include <X11/extensions/Xrandr.h>
#endif // HAVE_RANDR

// system headers

#ifdef HAVE_CSTDIO
  #include <cstdio>
#else
  #include <stdio.h>
#endif
#ifdef HAVE_CSTDLIB
  #include <cstdlib>
#else
  #include <stdlib.h>
#endif
#ifdef HAVE_CSTRING
  #include <cstring>
#else
  #include <string.h>
#endif

#ifdef HAVE_UNISTD_H
#include <sys/types.h>
#include <unistd.h>
#endif // HAVE_UNISTD_H

#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif // HAVE_SYS_PARAM_H

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif // HAVE_SYS_SELECT_H

#ifdef HAVE_SYS_STAT_H
#include <sys/types.h>
#include <sys/stat.h>
#endif // HAVE_SYS_STAT_H

#include <sys/wait.h>

#include <iostream>
#include <memory>
#include <algorithm>
#include <typeinfo>

using std::cerr;
using std::endl;
using std::string;
using std::vector;
using std::list;
using std::pair;
using std::bind2nd;
using std::mem_fun;
using std::equal_to;
using std::hex;
using std::dec;

using namespace FbTk;

namespace {

Window last_bad_window = None;

// *** NOTE: if you want to debug here the X errors are
//     coming from, you should turn on the XSynchronise call below
int handleXErrors(Display *d, XErrorEvent *e) {
    if (e->error_code == BadWindow)
        last_bad_window = e->resourceid;
#ifdef DEBUG
    else {
        // ignore bad window ones, they happen a lot
        // when windows close themselves
        char errtxt[128];

        XGetErrorText(d, e->error_code, errtxt, 128);
        cerr << "Fluxbox: X Error: " 
             << errtxt 
             << "(" <<(int)e->error_code << ") opcodes " 
             << (int)e->request_code 
             << "/" 
             << (int)e->minor_code 
             << " resource 0x" << hex <<(int)e->resourceid 
             << dec << endl;
//        if (e->error_code != 9 && e->error_code != 183)
//            kill(0, 2);
    }
#endif // !DEBUG

    return False;
}



/* functor to call a memberfunction with by a reference argument
   other places needs this helper as well it should be moved
   to FbTk/

   g++-4.1 does not like to work with:

      struct Bar;
      struct Foo {
          void foo(Bar&);
      };

      Bar bar;
      bind2nd(mem_fun(&F::foo), bar);

   it complaints about not beeing able to store a reference to
   a reference (Bar&&).

   'CallMemFunWithRefArg' makes g++-4.1 happy without
   having to consider switching over to boost::bind() or enforcing 
   a newer compiler.
 */
template <typename Type, typename ArgType, typename ResultType>
struct CallMemFunWithRefArg : std::unary_function<Type, ResultType> {

    explicit CallMemFunWithRefArg(ResultType (Type::*func)(ArgType), ArgType arg) :
        m_arg(arg),
        m_func(func) { }

    ResultType operator()(Type* p) const {
        (*p.*m_func)(m_arg);
    }

    ArgType m_arg;
    ResultType (Type::*m_func)(ArgType);
};


} // end anonymous

//static singleton var
Fluxbox *Fluxbox::s_singleton=0;

Fluxbox::Fluxbox(int argc, char **argv,
                 const std::string& dpy_name,
                 const std::string& rc_path, const std::string& rc_filename, bool xsync)
    : FbTk::App(dpy_name.c_str()),
      m_fbatoms(FbAtoms::instance()),
      m_resourcemanager(rc_filename.c_str(), true),
      // TODO: shouldn't need a separate one for screen
      m_screen_rm(m_resourcemanager),

      m_RC_PATH(rc_path),
      m_RC_INIT_FILE("init"),
      m_rc_ignoreborder(m_resourcemanager, false, "session.ignoreBorder", "Session.IgnoreBorder"),
      m_rc_pseudotrans(m_resourcemanager, false, "session.forcePseudoTransparency", "Session.forcePseudoTransparency"),
      m_rc_colors_per_channel(m_resourcemanager, 4,
                              "session.colorsPerChannel", "Session.ColorsPerChannel"),
      m_rc_double_click_interval(m_resourcemanager, 250, "session.doubleClickInterval", "Session.DoubleClickInterval"),
      m_rc_tabs_padding(m_resourcemanager, 0, "session.tabPadding", "Session.TabPadding"),
      m_rc_stylefile(m_resourcemanager, DEFAULTSTYLE, "session.styleFile", "Session.StyleFile"),
      m_rc_styleoverlayfile(m_resourcemanager, m_RC_PATH + "/overlay", "session.styleOverlay", "Session.StyleOverlay"),
      m_rc_menufile(m_resourcemanager, m_RC_PATH + "/menu", "session.menuFile", "Session.MenuFile"),
      m_rc_keyfile(m_resourcemanager, m_RC_PATH + "/keys", "session.keyFile", "Session.KeyFile"),
      m_rc_slitlistfile(m_resourcemanager, m_RC_PATH + "/slitlist", "session.slitlistFile", "Session.SlitlistFile"),
      m_rc_appsfile(m_resourcemanager, m_RC_PATH + "/apps", "session.appsFile", "Session.AppsFile"),
      m_rc_tabs_attach_area(m_resourcemanager, ATTACH_AREA_WINDOW, "session.tabsAttachArea", "Session.TabsAttachArea"),
      m_rc_cache_life(m_resourcemanager, 5, "session.cacheLife", "Session.CacheLife"),
      m_rc_cache_max(m_resourcemanager, 200, "session.cacheMax", "Session.CacheMax"),
      m_rc_auto_raise_delay(m_resourcemanager, 250, "session.autoRaiseDelay", "Session.AutoRaiseDelay"),
      m_masked_window(0),
      m_mousescreen(0),
      m_keyscreen(0),
      m_last_time(0),
      m_masked(0),
      m_rc_file(rc_filename),
      m_argv(argv), m_argc(argc),
      m_showing_dialog(false),
      m_starting(true),
      m_restarting(false),
      m_shutdown(false),
      m_server_grabs(0),
      m_randr_event_type(0) {

    _FB_USES_NLS;
    if (s_singleton != 0)
        throw _FB_CONSOLETEXT(Fluxbox, FatalSingleton, "Fatal! There can only one instance of fluxbox class.", "Error displayed on weird error where an instance of the Fluxbox class already exists!");

    if (display() == 0) {
        throw _FB_CONSOLETEXT(Fluxbox, NoDisplay,
                      "Can not connect to X server.\nMake sure you started X before you start Fluxbox.",
                      "Error message when no X display appears to exist");
    }

    Display *disp = FbTk::App::instance()->display();
    // For KDE dock applets
    // KDE v1.x
    m_kwm1_dockwindow = XInternAtom(disp,
                                    "KWM_DOCKWINDOW", False);
    // KDE v2.x
    m_kwm2_dockwindow = XInternAtom(disp,
                                    "_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR", False);
    // setup X error handler
    XSetErrorHandler((XErrorHandler) handleXErrors);

    //catch system signals
    SignalHandler &sigh = SignalHandler::instance();
    sigh.registerHandler(SIGSEGV, this);
    sigh.registerHandler(SIGFPE, this);
    sigh.registerHandler(SIGPIPE, this); // e.g. output sent to grep
    sigh.registerHandler(SIGTERM, this);
    sigh.registerHandler(SIGINT, this);
    sigh.registerHandler(SIGCHLD, this);
    sigh.registerHandler(SIGHUP, this);
    sigh.registerHandler(SIGUSR1, this);
    sigh.registerHandler(SIGUSR2, this);
    //
    // setup timer
    // This timer is used to we can issue a safe reconfig command.
    // Because when the command is executed we shouldn't do reconfig directly
    // because it could affect ongoing menu stuff so we need to reconfig in
    // the next event "round".
    FbTk::RefCount<FbTk::Command<void> > reconfig_cmd(new FbTk::SimpleCommand<Fluxbox>(*this, &Fluxbox::timed_reconfigure));
    m_reconfig_timer.setTimeout(0, 1);
    m_reconfig_timer.setCommand(reconfig_cmd);
    m_reconfig_timer.fireOnce(true);

    if (xsync)
        XSynchronize(disp, True);

    s_singleton = this;
    m_have_shape = false;
    m_shape_eventbase = 0;
#ifdef SHAPE
    int shape_err;
    m_have_shape = XShapeQueryExtension(disp, &m_shape_eventbase, &shape_err);
#endif // SHAPE

#ifdef HAVE_RANDR
    // get randr event type
    int randr_error_base;
    XRRQueryExtension(disp, &m_randr_event_type, &randr_error_base);
#endif // HAVE_RANDR

    load_rc();

    grab();

    if (! XSupportsLocale())
        cerr<<_FB_CONSOLETEXT(Fluxbox, WarningLocale, 
                              "Warning: X server does not support locale", 
                              "XSupportsLocale returned false")<<endl;

    if (XSetLocaleModifiers("") == 0)
        cerr<<_FB_CONSOLETEXT(Fluxbox, WarningLocaleModifiers, 
                              "Warning: cannot set locale modifiers", 
                              "XSetLocaleModifiers returned false")<<endl;


#ifdef HAVE_GETPID
    m_fluxbox_pid = XInternAtom(disp, "_BLACKBOX_PID", False);
#endif // HAVE_GETPID


    // setup theme manager to have our style file ready to be scanned
    FbTk::ThemeManager::instance().load(getStyleFilename(), getStyleOverlayFilename());

    // Create keybindings handler and load keys file
    // Note: this needs to be done before creating screens
    m_key.reset(new Keys);
    m_key->reconfigure();

    vector<int> screens;
    int i;

    // default is "use all screens"
    for (i = 0; i < ScreenCount(disp); i++)
        screens.push_back(i);

    // find out, on what "screens" fluxbox should run
    // FIXME(php-coder): maybe it worths moving this code to main.cc, where command line is parsed?
    for (i = 1; i < m_argc; i++) {
        if (! strcmp(m_argv[i], "-screen")) {
            if ((++i) >= m_argc) {
                cerr << _FB_CONSOLETEXT(main, ScreenRequiresArg, 
                                        "error, -screen requires argument", 
                                        "the -screen option requires a file argument") << endl;
                exit(EXIT_FAILURE);
            }

            // "all" is default
            if (! strcmp(m_argv[i], "all"))
                break;

            vector<string> vals;
            vector<int> scrtmp;
            int scrnr = 0;
            FbTk::StringUtil::stringtok(vals, m_argv[i], ",:");
            for (vector<string>::iterator scrit = vals.begin();
                 scrit != vals.end(); scrit++) {
                scrnr = atoi(scrit->c_str());
                if (scrnr >= 0 && scrnr < ScreenCount(disp))
                    scrtmp.push_back(scrnr);
            }

            if (!vals.empty())
                swap(scrtmp, screens);
        }
    }

    // create screens
    for (size_t s = 0; s < screens.size(); s++) {
        std::string sc_nr = FbTk::StringUtil::number2String(screens[s]);
        BScreen *screen = new BScreen(m_screen_rm.lock(),
                                      std::string("session.screen") + sc_nr,
                                      std::string("session.Screen") + sc_nr,
                                      screens[s], ::ResourceLayer::NUM_LAYERS);

        // already handled
        if (! screen->isScreenManaged()) {
            delete screen;
            continue;
        }

        // add to our list
        m_screen_list.push_back(screen);
    }

    if (m_screen_list.empty()) {
        throw _FB_CONSOLETEXT(Fluxbox, ErrorNoScreens,
                             "Couldn't find screens to manage.\nMake sure you don't have another window manager running.",
                             "Error message when no unmanaged screens found - usually means another window manager is running");
    }

    m_keyscreen = m_mousescreen = m_screen_list.front();

#ifdef USE_NEWWMSPEC
    addAtomHandler(new Ewmh()); // for Extended window manager atom support
#endif // USE_NEWWMSPEC
    // parse apps file after creating screens (so we can tell if it's a restart
    // for [startup] items) but before creating windows
    // this needs to be after ewmh and gnome, so state atoms don't get
    // overwritten before they're applied
#ifdef REMEMBER
    addAtomHandler(new Remember()); // for remembering window attribs
#endif // REMEMBER

    // init all "screens"
    STLUtil::forAll(m_screen_list, bind1st(mem_fun(&Fluxbox::initScreen), this));

    XAllowEvents(disp, ReplayPointer, CurrentTime);

    //XSynchronize(disp, False);
    sync(false);

    m_reconfigure_wait = false;

    m_resourcemanager.unlock();
    ungrab();

    if (m_resourcemanager.lockDepth() != 0) {
        fbdbg<<"--- resource manager lockdepth = "<<m_resourcemanager.lockDepth()<<endl;
    }

    m_starting = false;
    //
    // For dumping theme items
    // FbTk::ThemeManager::instance().listItems();
    //
    //    m_resourcemanager.dump();

}


Fluxbox::~Fluxbox() {

    // this needs to be destroyed before screens; otherwise, menus stored in
    // key commands cause a segfault when the LayerItem is destroyed
    m_key.reset(0);

    leaveAll(); // leave all connections

    // destroy screens (after others, as they may do screen things)
    FbTk::STLUtil::destroyAndClear(m_screen_list);

    FbTk::STLUtil::destroyAndClear(m_atomhandler);
}


void Fluxbox::initScreen(BScreen *screen) {

    // now we can create menus (which needs this screen to be in screen_list)
    screen->initMenus();
    screen->initWindows();

    // attach screen signals to this
    join(screen->workspaceAreaSig(),
         FbTk::MemFun(*this, &Fluxbox::workspaceAreaChanged));

    join(screen->focusedWindowSig(),
         FbTk::MemFun(*this, &Fluxbox::focusedWindowChanged));

    join(screen->clientListSig(),
         FbTk::MemFun(*this, &Fluxbox::clientListChanged));

    join(screen->workspaceNamesSig(), 
         FbTk::MemFun(*this, &Fluxbox::workspaceNamesChanged));
    join(screen->currentWorkspaceSig(), 
         FbTk::MemFun(*this, &Fluxbox::workspaceChanged));

    join(screen->workspaceCountSig(), 
         FbTk::MemFun(*this, &Fluxbox::workspaceCountChanged));

    // initiate atomhandler for screen specific stuff
    STLUtil::forAll(m_atomhandler, 
            CallMemFunWithRefArg<AtomHandler, BScreen&, void>(&AtomHandler::initForScreen, *screen));
    //STLUtil::forAll(m_atomhandler, bind2nd(mem_fun(&AtomHandler::initForScreen), *screen));

    FocusControl::revertFocus(*screen); // make sure focus style is correct

}


void Fluxbox::eventLoop() {
    Display *disp = display();
    while (!m_shutdown) {
        if (XPending(disp)) {
            XEvent e;
            XNextEvent(disp, &e);

            if (last_bad_window != None && e.xany.window == last_bad_window &&
                e.type != DestroyNotify) { // we must let the actual destroys through
                if (e.type == FocusOut)
                    revertFocus();
                else
                    fbdbg<<"Fluxbox::eventLoop(): removing bad window from event queue"<<endl;
            } else {
                last_bad_window = None;
                handleEvent(&e);
            }
        } else {
            FbTk::Timer::updateTimers(ConnectionNumber(disp)); //handle all timers
        }
    }
}

bool Fluxbox::validateWindow(Window window) const {
    XEvent event;
    if (XCheckTypedWindowEvent(display(), window, DestroyNotify, &event)) {
        XPutBackEvent(display(), &event);
        return false;
    }

    return true;
}

void Fluxbox::grab() {
    if (! m_server_grabs++)
       XGrabServer(display());
}

void Fluxbox::ungrab() {
    if (! --m_server_grabs)
        XUngrabServer(display());

    if (m_server_grabs < 0)
        m_server_grabs = 0;
}

void Fluxbox::handleEvent(XEvent * const e) {
    _FB_USES_NLS;
    m_last_event = *e;

    // it is possible (e.g. during moving) for a window
    // to mask all events to go to it
    if ((m_masked == e->xany.window) && m_masked_window) {
        if (e->type == MotionNotify) {
            m_last_time = e->xmotion.time;
            m_masked_window->motionNotifyEvent(e->xmotion);
            return;
        } else if (e->type == ButtonRelease) {
            e->xbutton.window = m_masked_window->fbWindow().window();
        }

    }

    // update key/mouse screen and last time before we enter other eventhandlers
    if (e->type == KeyPress ||
        e->type == KeyRelease) {
        m_keyscreen = searchScreen(e->xkey.root);
    } else if (e->type == ButtonPress ||
               e->type == ButtonRelease ||
               e->type == MotionNotify ) {
        if (e->type == MotionNotify)
            m_last_time = e->xmotion.time;
        else
            m_last_time = e->xbutton.time;

        m_mousescreen = searchScreen(e->xbutton.root);
    } else if (e->type == EnterNotify ||
               e->type == LeaveNotify) {
        m_last_time = e->xcrossing.time;
        m_mousescreen = searchScreen(e->xcrossing.root);
    } else if (e->type == PropertyNotify) {
        m_last_time = e->xproperty.time;
        // check transparency atoms if it's a root pm

        BScreen *screen = searchScreen(e->xproperty.window);
        if (screen) {
            screen->propertyNotify(e->xproperty.atom);
        }
    }

    // try FbTk::EventHandler first
    FbTk::EventManager::instance()->handleEvent(*e);

    switch (e->type) {
    case ButtonRelease:
    case ButtonPress:
        break;
    case ConfigureRequest: {

        if (!searchWindow(e->xconfigurerequest.window)) {

            grab();

            if (validateWindow(e->xconfigurerequest.window)) {
                XWindowChanges xwc;

                xwc.x = e->xconfigurerequest.x;
                xwc.y = e->xconfigurerequest.y;
                xwc.width = e->xconfigurerequest.width;
                xwc.height = e->xconfigurerequest.height;
                xwc.border_width = e->xconfigurerequest.border_width;
                xwc.sibling = e->xconfigurerequest.above;
                xwc.stack_mode = e->xconfigurerequest.detail;

                XConfigureWindow(FbTk::App::instance()->display(),
                                 e->xconfigurerequest.window,
                                 e->xconfigurerequest.value_mask, &xwc);
            }

            ungrab();
        } // else already handled in FluxboxWindow::handleEvent

    }
        break;
    case MapRequest: {

        fbdbg<<"MapRequest for 0x"<<hex<<e->xmaprequest.window<<dec<<endl;

        WinClient *winclient = searchWindow(e->xmaprequest.window);

        if (! winclient) {
            BScreen *screen = 0;
            int screen_num;
            XWindowAttributes attr;
            // find screen
            if (XGetWindowAttributes(display(),
                                     e->xmaprequest.window,
                                     &attr) && attr.screen != 0) {
                screen_num = XScreenNumberOfScreen(attr.screen);

                // find screen
                ScreenList::iterator screen_it = find_if(m_screen_list.begin(),
                                                         m_screen_list.end(),
                                                         FbTk::CompareEqual<BScreen>(&BScreen::screenNumber, screen_num));
                if (screen_it != m_screen_list.end())
                    screen = *screen_it;
            }
            // try with parent if we failed to find screen num
            if (screen == 0)
               screen = searchScreen(e->xmaprequest.parent);

            if (screen == 0) {
                cerr<<"Fluxbox "<<_FB_CONSOLETEXT(Fluxbox, CantMapWindow, "Warning! Could not find screen to map window on!", "")<<endl;
            } else
                screen->createWindow(e->xmaprequest.window);

        } else {
            // we don't handle MapRequest in FluxboxWindow::handleEvent
            if (winclient->fbwindow())
                winclient->fbwindow()->mapRequestEvent(e->xmaprequest);
        }

    }
        break;
    case MapNotify:
        // handled directly in FluxboxWindow::handleEvent
        break;
    case UnmapNotify:
        handleUnmapNotify(e->xunmap);
	break;
    case MappingNotify:
        // Update stored modifier mapping
        fbdbg<<"MappingNotify"<<endl;

        if (e->xmapping.request == MappingKeyboard
            || e->xmapping.request == MappingModifier) {
            XRefreshKeyboardMapping(&e->xmapping);
            FbTk::KeyUtil::instance().init(); // reinitialise the key utils
            // reconfigure keys (if the mapping changes, they don't otherwise update
            m_key->regrab();
        }
        break;
    case CreateNotify:
	break;
    case DestroyNotify: {
        WinClient *winclient = searchWindow(e->xdestroywindow.window);
        if (winclient != 0) {
            FluxboxWindow *win = winclient->fbwindow();
            if (win)
                win->destroyNotifyEvent(e->xdestroywindow);
        }

    }
        break;
    case MotionNotify:
        m_last_time = e->xmotion.time;
        break;
    case PropertyNotify: {
        m_last_time = e->xproperty.time;
        WinClient *winclient = searchWindow(e->xproperty.window);
        if (winclient == 0)
            break;
        // most of them are handled in FluxboxWindow::handleEvent
        // but some special cases like ewmh propertys needs to be checked
        for (AtomHandlerContainerIt it= m_atomhandler.begin();
             it != m_atomhandler.end(); it++) {
            if ( (*it)->propertyNotify(*winclient, e->xproperty.atom))
                break;
        }
    } break;
    case EnterNotify: {

        m_last_time = e->xcrossing.time;

        if (e->xcrossing.mode == NotifyGrab)
            break;

        BScreen *screen = 0;
        if ((e->xcrossing.window == e->xcrossing.root) &&
            (screen = searchScreen(e->xcrossing.window))) {
            screen->imageControl().installRootColormap();

        }

    } break;
    case LeaveNotify:
        m_last_time = e->xcrossing.time;
        break;
    case Expose:
        break;
    case KeyRelease:
    case KeyPress:
        break;
    case ColormapNotify: {
        BScreen *screen = searchScreen(e->xcolormap.window);

        if (screen != 0) {
            screen->setRootColormapInstalled((e->xcolormap.state ==
                                              ColormapInstalled) ? true : false);
        }
    } break;
    case FocusIn: {

        // a grab is something of a pseudo-focus event, so we ignore
        // them, here we ignore some window receiving it
        if (e->xfocus.mode == NotifyGrab ||
            e->xfocus.mode == NotifyUngrab ||
            e->xfocus.detail == NotifyPointer ||
            e->xfocus.detail == NotifyInferior)
            break;

        if (FbTk::Menu::focused() &&
            FbTk::Menu::focused()->window() == e->xfocus.window) {
            m_keyscreen = findScreen(FbTk::Menu::focused()->screenNumber());
            FocusControl::setFocusedWindow(0);
            break;
        }

        WinClient *winclient = searchWindow(e->xfocus.window);
        if (winclient)
            m_keyscreen = &winclient->screen();
        FocusControl::setFocusedWindow(winclient);

    } break;
    case FocusOut:{
        // and here we ignore some window losing the special grab focus
        if (e->xfocus.mode == NotifyGrab ||
            e->xfocus.detail == NotifyPointer ||
            e->xfocus.detail == NotifyInferior)
            break;

        WinClient *winclient = searchWindow(e->xfocus.window);
        if ((winclient == FocusControl::focusedWindow() ||
             FocusControl::focusedWindow() == 0) &&
            // we don't unfocus a moving window
            (!winclient || !winclient->fbwindow() ||
             !winclient->fbwindow()->isMoving()))
            revertFocus();
    }
        break;
    case ClientMessage:
        handleClientMessage(e->xclient);
        break;
    default: {

#ifdef HAVE_RANDR
        if (e->type == m_randr_event_type) {
#ifdef HAVE_RANDR1_2
            XRRUpdateConfiguration(e);
#endif
            // update root window size in screen
            BScreen *scr = searchScreen(e->xany.window);
            if (scr != 0)
                scr->updateSize();
        }
#endif // HAVE_RANDR

    }

    }
}

void Fluxbox::handleUnmapNotify(XUnmapEvent &ue) {

    BScreen *screen = searchScreen(ue.event);

    if (screen) {
        /* Ignore all EnterNotify events until the pointer actually moves */
        screen->focusControl().ignoreAtPointer();
    }

    if (ue.event != ue.window && (!screen || !ue.send_event)) {
        return;
    }

    WinClient *winclient = searchWindow(ue.window);

    if (winclient != 0) {

        FluxboxWindow *win = winclient->fbwindow();
        if (!win) {
            delete winclient;
            return;
        }

        // this should delete client and adjust m_focused_window if necessary
        win->unmapNotifyEvent(ue);

    // according to http://tronche.com/gui/x/icccm/sec-4.html#s-4.1.4
    // a XWithdrawWindow is
    //   1) unmapping the window (which leads to the upper branch
    //   2) sends an synthetic unampevent (which is handled below)
    } else if (screen && ue.send_event) {
        XDeleteProperty(display(), ue.window, FbAtoms::instance()->getWMStateAtom());
        XUngrabButton(display(), AnyButton, AnyModifier, ue.window);
    }

}

/**
 * Handles XClientMessageEvent
 */
void Fluxbox::handleClientMessage(XClientMessageEvent &ce) {

#ifdef DEBUG
    char * atom = 0;
    if (ce.message_type)
        atom = XGetAtomName(FbTk::App::instance()->display(), ce.message_type);

    fbdbg<<__FILE__<<"("<<__LINE__<<"): ClientMessage. data.l[0]=0x"<<hex<<ce.data.l[0]<<
	"  message_type=0x"<<ce.message_type<<dec<<" = \""<<atom<<"\""<<endl;

    if (ce.message_type && atom) XFree((char *) atom);
#endif // DEBUG


    if (ce.format != 32)
        return;

    if (ce.message_type == m_fbatoms->getWMChangeStateAtom()) {
        WinClient *winclient = searchWindow(ce.window);
        if (! winclient || !winclient->fbwindow() || ! winclient->validateClient())
            return;

        if (ce.data.l[0] == IconicState)
            winclient->fbwindow()->iconify();
        if (ce.data.l[0] == NormalState)
            winclient->fbwindow()->deiconify();
    } else {
        WinClient *winclient = searchWindow(ce.window);
        BScreen *screen = searchScreen(ce.window);
        // note: we dont need screen nor winclient to be non-null,
        // it's up to the atomhandler to check that
        for (AtomHandlerContainerIt it= m_atomhandler.begin();
             it != m_atomhandler.end(); it++) {
            (*it)->checkClientMessage(ce, screen, winclient);
        }

    }
}

/// handle system signals
void Fluxbox::handleSignal(int signum) {
    _FB_USES_NLS;

    static int re_enter = 0;

    switch (signum) {
    case SIGCHLD: // we don't want the child process to kill us
        // more than one process may have terminated
        while (waitpid(-1, 0, WNOHANG | WUNTRACED) > 0);
        break;
    case SIGHUP:
        restart();
        break;
    case SIGUSR1:
        load_rc();
        break;
    case SIGUSR2:
        reconfigure();
        break;
    case SIGSEGV:
        abort();
        break;
    case SIGFPE:
    case SIGINT:
    case SIGPIPE:
    case SIGTERM:
        shutdown();
        break;
    default:
        fprintf(stderr,
                _FB_CONSOLETEXT(BaseDisplay, SignalCaught, "%s:      signal %d caught\n", "signal catch debug message. Include %s for Command<void> and %d for signal number").c_str(),
                m_argv[0], signum);

        if (! m_starting && ! re_enter) {
            re_enter = 1;
            cerr<<_FB_CONSOLETEXT(BaseDisplay, ShuttingDown, "Shutting Down\n", "Quitting because of signal, end with newline");
            shutdown();
        }


        cerr<<_FB_CONSOLETEXT(BaseDisplay, Aborting, "Aborting... dumping core\n", "Aboring and dumping core, end with newline");
        abort();
        break;
    }
}


void Fluxbox::windowDied(Focusable &focusable) {
    FluxboxWindow *fbwin = focusable.fbwindow();

    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
        CallMemFunWithRefArg<AtomHandler, FluxboxWindow&, void>(&AtomHandler::updateFrameClose, *focusable.fbwindow()));

    // make sure each workspace get this
    BScreen &scr = focusable.screen();
    scr.removeWindow(fbwin);
    if (FocusControl::focusedFbWindow() == fbwin)
        FocusControl::setFocusedFbWindow(0);
}

void Fluxbox::clientDied(Focusable &focusable) {
    WinClient &client = dynamic_cast<WinClient &>(focusable);

    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
            CallMemFunWithRefArg<AtomHandler, WinClient&, void>(&AtomHandler::updateClientClose, client));

    BScreen &screen = client.screen();

    // At this point, we trust that this client is no longer in the
    // client list of its frame (but it still has reference to the frame)
    // We also assume that any remaining active one is the last focused one

    // This is where we revert focus on window close
    // NOWHERE ELSE!!!
    if (FocusControl::focusedWindow() == &client) {
        FocusControl::unfocusWindow(client);
        // make sure nothing else uses this window before focus reverts
        FocusControl::setFocusedWindow(0);
    } else if (FocusControl::expectingFocus() == &client) {
        FocusControl::setExpectingFocus(0);
        revertFocus();
    }

    screen.removeClient(client);
}

void Fluxbox::windowWorkspaceChanged(FluxboxWindow &win) {
    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
        CallMemFunWithRefArg<AtomHandler, FluxboxWindow&, void>(&AtomHandler::updateWorkspace, win));
}

void Fluxbox::windowStateChanged(FluxboxWindow &win) {
    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
        CallMemFunWithRefArg<AtomHandler, FluxboxWindow&, void>(&AtomHandler::updateState, win));

    // if window changed to iconic state
    // add to icon list
    if (win.isIconic()) {
        win.screen().addIcon(&win);
        Workspace *space = win.screen().getWorkspace(win.workspaceNumber());
        if (space != 0)
            space->removeWindow(&win, true);
    }

    if (win.isStuck()) {
        // if we're sticky then reassociate window
        // to all workspaces
        BScreen &scr = win.screen();
        if (scr.currentWorkspaceID() != win.workspaceNumber())
            scr.reassociateWindow(&win, scr.currentWorkspaceID(), true);
    }
}

void Fluxbox::windowLayerChanged(FluxboxWindow &win) {
    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
        CallMemFunWithRefArg<AtomHandler, FluxboxWindow&, void>(&AtomHandler::updateLayer, win));
}

void Fluxbox::attachSignals(FluxboxWindow &win) {
    join(win.stateSig(), FbTk::MemFun(*this, &Fluxbox::windowStateChanged));
    join(win.workspaceSig(), FbTk::MemFun(*this, &Fluxbox::windowWorkspaceChanged));
    join(win.layerSig(), FbTk::MemFun(*this, &Fluxbox::windowLayerChanged));
    join(win.dieSig(), FbTk::MemFun(*this, &Fluxbox::windowDied));
    STLUtil::forAll(m_atomhandler,
            CallMemFunWithRefArg<AtomHandler, FluxboxWindow&, void>(&AtomHandler::setupFrame, win));
}

void Fluxbox::attachSignals(WinClient &winclient) {
    join(winclient.dieSig(), FbTk::MemFun(*this, &Fluxbox::clientDied));
    STLUtil::forAll(m_atomhandler,
            CallMemFunWithRefArg<AtomHandler, WinClient&, void>(&AtomHandler::setupClient, winclient));
}

BScreen *Fluxbox::searchScreen(Window window) {

    ScreenList::iterator it = m_screen_list.begin();
    ScreenList::iterator it_end = m_screen_list.end();
    for (; it != it_end; ++it) {
        if (*it && (*it)->rootWindow() == window)
            return *it;
    }

    return 0;
}


AtomHandler* Fluxbox::getAtomHandler(const string &name) {
    if ( name != "" ) {

        AtomHandlerContainerIt it;
        for (it = m_atomhandler.begin(); it != m_atomhandler.end(); it++) {
            if (name == (*it)->getName())
                return *it;
        }
    }
    return 0;
}
void Fluxbox::addAtomHandler(AtomHandler *atomh) {
    m_atomhandler.insert(atomh);
}

void Fluxbox::removeAtomHandler(AtomHandler *atomh) {
    m_atomhandler.erase(atomh);
}

WinClient *Fluxbox::searchWindow(Window window) {
    WinClientMap::iterator it = m_window_search.find(window);
    if (it != m_window_search.end())
        return it->second;

    WindowMap::iterator git = m_window_search_group.find(window);
    return git == m_window_search_group.end() ? 0 : &git->second->winClient();
}


/* Not implemented until we know how it'll be used
 * Recall that this refers to ICCCM groups, not fluxbox tabgroups
 * See ICCCM 4.1.11 for details
 */
/*
WinClient *Fluxbox::searchGroup(Window window) {
}
*/

void Fluxbox::saveWindowSearch(Window window, WinClient *data) {
    m_window_search[window] = data;
}

/* some windows relate to the whole group */
void Fluxbox::saveWindowSearchGroup(Window window, FluxboxWindow *data) {
    m_window_search_group[window] = data;
}

void Fluxbox::saveGroupSearch(Window window, WinClient *data) {
    m_group_search.insert(pair<const Window, WinClient *>(window, data));
}


void Fluxbox::removeWindowSearch(Window window) {
    m_window_search.erase(window);
}

void Fluxbox::removeWindowSearchGroup(Window window) {
    m_window_search_group.erase(window);
}

void Fluxbox::removeGroupSearch(Window window) {
    m_group_search.erase(window);
}

/// restarts fluxbox
void Fluxbox::restart(const char *prog) {
    shutdown();

    m_restarting = true;

    if (prog && *prog != '\0') {
        m_restart_argument = prog;
    }
}

/// prepares fluxbox for a shutdown
void Fluxbox::shutdown() {
    if (m_shutdown)
        return;

    m_shutdown = true;

    XSetInputFocus(FbTk::App::instance()->display(), PointerRoot, None, CurrentTime);

    STLUtil::forAll(m_screen_list, mem_fun(&BScreen::shutdown));

    sync(false);
}

/// saves resources
void Fluxbox::save_rc() {
    _FB_USES_NLS;
    XrmDatabase new_rc = 0;

    string dbfile(getRcFilename());

    if (!dbfile.empty()) {
        m_resourcemanager.save(dbfile.c_str(), dbfile.c_str());
        m_screen_rm.save(dbfile.c_str(), dbfile.c_str());
    } else
        cerr<<_FB_CONSOLETEXT(Fluxbox, BadRCFile, "rc filename is invalid!", "Bad settings file")<<endl;


    ScreenList::iterator it = m_screen_list.begin();
    ScreenList::iterator it_end = m_screen_list.end();
    for (; it != it_end; ++it) {
        BScreen *screen = *it;

        std::string workspaces_string("session.screen");
        workspaces_string += FbTk::StringUtil::number2String(screen->screenNumber());
        workspaces_string += ".workspaceNames: ";

        // these are static, but may not be saved in the users resource file,
        // writing these resources will allow the user to edit them at a later
        // time... but loading the defaults before saving allows us to rewrite the
        // users changes...

        const BScreen::WorkspaceNames& names = screen->getWorkspaceNames();
        for (size_t i=0; i < names.size(); i++) {
            workspaces_string += FbTk::FbStringUtil::FbStrToLocale(names[i]);
            workspaces_string += ',';
        }

        XrmPutLineResource(&new_rc, workspaces_string.c_str());

    }

    XrmDatabase old_rc = XrmGetFileDatabase(dbfile.c_str());

    XrmMergeDatabases(new_rc, &old_rc);
    XrmPutFileDatabase(old_rc, dbfile.c_str());
    XrmDestroyDatabase(old_rc);

    fbdbg<<__FILE__<<"("<<__LINE__<<"): ------------ SAVING DONE"<<endl;

}

/// @return filename of resource file
string Fluxbox::getRcFilename() {
    if (m_rc_file.empty())
        return getDefaultDataFilename(m_RC_INIT_FILE);
    return m_rc_file;
}

/// Provides default filename of data file
string Fluxbox::getDefaultDataFilename(const char *name) const {
    return m_RC_PATH + string("/") + name;
}

/// loads resources
void Fluxbox::load_rc() {
    _FB_USES_NLS;

    string dbfile(getRcFilename());

    if (!dbfile.empty()) {
        if (!m_resourcemanager.load(dbfile.c_str())) {
            cerr<<_FB_CONSOLETEXT(Fluxbox, CantLoadRCFile, "Failed to load database", "Failed trying to read rc file")<<":"<<dbfile<<endl;
            cerr<<_FB_CONSOLETEXT(Fluxbox, CantLoadRCFileTrying, "Retrying with", "Retrying rc file loading with (the following file)")<<": "<<DEFAULT_INITFILE<<endl;
            if (!m_resourcemanager.load(DEFAULT_INITFILE))
                cerr<<_FB_CONSOLETEXT(Fluxbox, CantLoadRCFile, "Failed to load database", "")<<": "<<DEFAULT_INITFILE<<endl;
        }
    } else {
        if (!m_resourcemanager.load(DEFAULT_INITFILE))
            cerr<<_FB_CONSOLETEXT(Fluxbox, CantLoadRCFile, "Failed to load database", "")<<": "<<DEFAULT_INITFILE<<endl;
    }

    if (m_rc_menufile->empty())
        m_rc_menufile.setDefaultValue();

    FbTk::Transparent::usePseudoTransparent(*m_rc_pseudotrans);

    if (!m_rc_slitlistfile->empty()) {
        *m_rc_slitlistfile = StringUtil::expandFilename(*m_rc_slitlistfile);
    } else {
        string filename = getDefaultDataFilename("slitlist");
        m_rc_slitlistfile.setFromString(filename.c_str());
    }

    *m_rc_colors_per_channel = FbTk::Util::clamp(*m_rc_colors_per_channel, 2, 6);

    if (m_rc_stylefile->empty())
        *m_rc_stylefile = DEFAULTSTYLE;
}

void Fluxbox::load_rc(BScreen &screen) {
    //get resource filename
    _FB_USES_NLS;
    string dbfile(getRcFilename());

    XrmDatabaseHelper database;

    database = XrmGetFileDatabase(dbfile.c_str());
    if (database==0)
        database = XrmGetFileDatabase(DEFAULT_INITFILE);


    screen.removeWorkspaceNames();

    std::string screen_number = FbTk::StringUtil::number2String(screen.screenNumber());

    std::string name_lookup("session.screen");
    name_lookup += screen_number;
    name_lookup += ".workspaceNames";
    std::string class_lookup("session.screen");
    class_lookup += screen_number;
    class_lookup += ".WorkspaceNames";

    XrmValue value;
    char *value_type;
    if (XrmGetResource(*database, name_lookup.c_str(), class_lookup.c_str(), &value_type,
                       &value)) {

        string values(value.addr);
        BScreen::WorkspaceNames names;

        StringUtil::removeTrailingWhitespace(values);
        StringUtil::removeFirstWhitespace(values);
        StringUtil::stringtok<BScreen::WorkspaceNames>(names, values, ",");
        BScreen::WorkspaceNames::iterator it;
        for(it = names.begin(); it != names.end(); it++) {
            if (!(*it).empty() && (*it) != "")
            screen.addWorkspaceName((*it).c_str());
        }

    }

    if (!dbfile.empty()) {
        if (!m_screen_rm.load(dbfile.c_str())) {
            cerr<<_FB_CONSOLETEXT(Fluxbox, CantLoadRCFile, "Failed to load database", "Failed trying to read rc file")<<":"<<dbfile<<endl;
            cerr<<_FB_CONSOLETEXT(Fluxbox, CantLoadRCFileTrying, "Retrying with", "Retrying rc file loading with (the following file)")<<": "<<DEFAULT_INITFILE<<endl;
            if (!m_screen_rm.load(DEFAULT_INITFILE))
                cerr<<_FB_CONSOLETEXT(Fluxbox, CantLoadRCFile, "Failed to load database", "")<<": "<<DEFAULT_INITFILE<<endl;
        }
    } else {
        if (!m_screen_rm.load(DEFAULT_INITFILE))
            cerr<<_FB_CONSOLETEXT(Fluxbox, CantLoadRCFile, "Failed to load database", "")<<": "<<DEFAULT_INITFILE<<endl;
    }
}

void Fluxbox::reconfigure() {
    load_rc();
    m_reconfigure_wait = true;
    m_reconfig_timer.start();
}


void Fluxbox::real_reconfigure() {

    FbTk::Transparent::usePseudoTransparent(*m_rc_pseudotrans);

    ScreenList::iterator screen_it = m_screen_list.begin();
    ScreenList::iterator screen_it_end = m_screen_list.end();
    for (; screen_it != screen_it_end; ++screen_it)
        load_rc(*(*screen_it));

    STLUtil::forAll(m_screen_list, mem_fun(&BScreen::reconfigure));
    m_key->reconfigure();
    STLUtil::forAll(m_atomhandler, mem_fun(&AtomHandler::reconfigure));
}

BScreen *Fluxbox::findScreen(int id) {

    BScreen* result = 0;
    ScreenList::iterator it = find_if(m_screen_list.begin(), m_screen_list.end(),
            FbTk::CompareEqual<BScreen>(&BScreen::screenNumber, id));

    if (it != m_screen_list.end())
        result = *it;

    return result;
}

void Fluxbox::timed_reconfigure() {
    if (m_reconfigure_wait)
        real_reconfigure();

    m_reconfigure_wait = false;
}

void Fluxbox::revertFocus() {
    bool revert = m_keyscreen && !m_showing_dialog;

    if (revert) {
        // see if there are any more focus events in the queue
        XEvent ev;
        while (XCheckMaskEvent(display(), FocusChangeMask, &ev))
            handleEvent(&ev);
        if (FocusControl::focusedWindow() || FocusControl::expectingFocus())
            return; // already handled

        Window win;
        int blah;
        XGetInputFocus(display(), &win, &blah);

        // we only want to revert focus if it's left dangling, as some other
        // application may have set the focus to an unmanaged window
        if (win != None && win != PointerRoot && !searchWindow(win) &&
            win != m_keyscreen->rootWindow().window())
            revert = false;
    }

    if (revert)
        FocusControl::revertFocus(*m_keyscreen);
    else
        FocusControl::setFocusedWindow(0);
}

bool Fluxbox::validateClient(const WinClient *client) const {
    WinClientMap::const_iterator it =
        find_if(m_window_search.begin(),
                m_window_search.end(),
                Compose(bind2nd(equal_to<WinClient *>(), client),
                        Select2nd<WinClientMap::value_type>()));
    return it != m_window_search.end();
}

void Fluxbox::updateFrameExtents(FluxboxWindow &win) {
    STLUtil::forAll(m_atomhandler, 
            CallMemFunWithRefArg<AtomHandler, FluxboxWindow&, void>(&AtomHandler::updateFrameExtents, win));
}

void Fluxbox::workspaceCountChanged( BScreen& screen ) {
    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
            CallMemFunWithRefArg<AtomHandler, BScreen&, void>(&AtomHandler::updateWorkspaceCount, screen));
}

void Fluxbox::workspaceChanged( BScreen& screen ) {
    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
            CallMemFunWithRefArg<AtomHandler, BScreen&, void>(&AtomHandler::updateCurrentWorkspace, screen));
}

void Fluxbox::workspaceNamesChanged(BScreen &screen) {
    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
            CallMemFunWithRefArg<AtomHandler, BScreen&, void>(&AtomHandler::updateWorkspaceNames, screen));
}

void Fluxbox::clientListChanged(BScreen &screen) {
    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
            CallMemFunWithRefArg<AtomHandler, BScreen&, void>(&AtomHandler::updateClientList, screen));
}

void Fluxbox::focusedWindowChanged(BScreen &screen, 
                                   FluxboxWindow* win, 
                                   WinClient* client) {

    for (AtomHandlerContainerIt it= m_atomhandler.begin();
         it != m_atomhandler.end(); it++) {
        (*it)->updateFocusedWindow(screen, client ? client->window() : 0 );
    }
}

void Fluxbox::workspaceAreaChanged(BScreen &screen) {
    STLUtil::forAllIf(m_atomhandler, mem_fun(&AtomHandler::update),
            CallMemFunWithRefArg<AtomHandler, BScreen&, void>(&AtomHandler::updateWorkarea, screen));
}

