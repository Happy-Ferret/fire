#include <commonincludes.hpp>
#include <opengl.hpp>
#include <winstack.hpp>
#include <wm.hpp>

#include <sstream>
#include <memory>

bool wmDetected;

Core *core;
int refreshrate;

class CorePlugin : public Plugin {
    public:
        void init() {
            options.insert(newIntOption("rrate", 100));
            options.insert(newIntOption("vwidth", 3));
            options.insert(newIntOption("vheight", 3));
            options.insert(newStringOption("background", ""));
            options.insert(newStringOption("shadersrc", "/usr/local/share/fireman/shaders"));
            options.insert(newStringOption("pluginpath", "/usr/local/lib/fireman/"));
            options.insert(newStringOption("plugins", ""));
        }
        void initOwnership() {
            owner->name = "core";
            owner->compatAll = true;
        }
        void updateConfiguration() {
            refreshrate = options["rrate"]->data.ival;
        }
};
PluginPtr plug; // used to get core options

Core::Core(int vx, int vy) {
    this->vx = vx;
    this->vy = vy;
}

void Core::enableInputPass(Window win) {
    XserverRegion region;
    region = XFixesCreateRegion(d, NULL, 0);
    XFixesSetWindowShapeRegion(d, win, ShapeBounding, 0, 0, 0);
    XFixesSetWindowShapeRegion(d, win, ShapeInput, 0, 0, region);
    XFixesDestroyRegion(d, region);
}

void Core::addExistingWindows() {
    Window dummy1, dummy2;
    uint size;
    Window  *children;

    XQueryTree(d, root, &dummy1, &dummy2, &children, &size);

    if(size == 0)
        return;

    for(int i = size - 1; i >= 0; i--)
        if(children[i] != overlay   &&
           children[i] != outputwin &&
           children[i] != s0owner   &&
           children[i] != root       )

            addWindow(children[i]);

    for(int i = size - 1; i >= 0; i--) {
        auto w = findWindow(children[i]);
        if(w) w->initialMapping = false,
              mapWindow(w, false);
    }
}

void Core::init() {
    d = XOpenDisplay(NULL);

    if ( d == nullptr )
        std::cout << "Failed to open display!" << std::endl;

    XSynchronize(d, 1);

    root = DefaultRootWindow(d);
    fd.fd = ConnectionNumber(d);
    fd.events = POLLIN;

    XSelectInput(d, root, SubstructureNotifyMask);

    XWindowAttributes xwa;
    XGetWindowAttributes(d, root, &xwa);
    width = xwa.width;
    height = xwa.height;

    XSetErrorHandler(&Core::onOtherWmDetected);

    XCompositeRedirectSubwindows(d, root, CompositeRedirectManual);
    XSelectInput (d, root,
             SubstructureRedirectMask | SubstructureNotifyMask   |
             StructureNotifyMask      | PropertyChangeMask       |
             LeaveWindowMask          | EnterWindowMask          |
             KeyPressMask             | KeyReleaseMask           |
             ButtonPressMask          | ButtonReleaseMask        |
             FocusChangeMask          | ExposureMask             );

    if(wmDetected)
       std::cout << "Another WM already running!\n", std::exit(-1);

    wins = new WinStack();

    using namespace std::placeholders;
    this->getWindowAtPoint =
        std::bind(std::mem_fn(&WinStack::findWindowAtCursorPosition),
                wins, _1, _2);

    XSetErrorHandler(&Core::onXError);
    overlay = XCompositeGetOverlayWindow(d, root);
    outputwin = GLXUtils::createNewWindowWithContext(overlay);

    enableInputPass(overlay);
    enableInputPass(outputwin);

    int dummy;
    XDamageQueryExtension(d, &damage, &dummy);

    cntHooks = 0;
    output = getMaximisedRegion();
    // enable compositing to be recognized by other programs
    Atom a;
    s0owner = XCreateSimpleWindow (d, root, 0, 0, 1, 1, 0, None, None);
    Xutf8SetWMProperties (d, s0owner,
            "xcompmgr", "xcompmgr",
            NULL, 0, NULL, NULL, NULL);

    a = XInternAtom (d, "_NET_WM_CM_S0", False);
    XSetSelectionOwner (d, a, s0owner, 0);

    run("setxkbmap -model pc104 -layout us,bg -variant ,phonetic -option grp:alt_shift_toggle");


    initDefaultPlugins();

    /* load core options */
    plug->owner = std::make_shared<_Ownership>();
    plug->initOwnership();
    plug->init();
    config->setOptionsForPlugin(plug);
    plug->updateConfiguration();

    vwidth = plug->options["vwidth"]->data.ival;
    vheight= plug->options["vheight"]->data.ival;

    loadDynamicPlugins();

    WinUtil::init();
    GLXUtils::initGLX();
    OpenGL::initOpenGL((*plug->options["shadersrc"]->data.sval).c_str());
    core->setBackground((*plug->options["background"]->data.sval).c_str());

    for(auto p : plugins) {
        p->owner = std::make_shared<_Ownership>();
        p->initOwnership();
        regOwner(p->owner);
        p->init();
        config->setOptionsForPlugin(p);
        p->updateConfiguration();
    }

    dmg = getMaximisedRegion();
    resetDMG = true;

    addExistingWindows();
    setDefaultRenderer();
    addDefaultSignals();
}

Core::~Core(){
    for(auto p : plugins) {
        p->fini();
        if(p->dynamic)
            dlclose(p->handle);
        p.reset();
    }

    XDestroyWindow(core->d, outputwin);
    XDestroyWindow(core->d, s0owner);

    XCompositeReleaseOverlayWindow(d, overlay);
}

void Core::run(const char *command) {
    auto pid = fork();

    if(!pid) {
        std::string str("DISPLAY=");
        str = str.append(XDisplayString(d));
        putenv(const_cast<char*>(str.c_str()));
        std::exit(execl("/bin/sh", "/bin/sh", "-c", command, NULL));
    }
}

Context::Context(XEvent ev) : xev(ev){}
Hook::Hook() : active(false) {}

void Core::addHook(Hook *hook){
    if(hook)
        hook->id = nextID++,
        hooks.push_back(hook);
}

void Core::remHook(uint key) {
    auto it = std::remove_if(hooks.begin(), hooks.end(), [key] (Hook *hook) {
                if(hook && hook->id == key) {
                        hook->disable();
                        return true;
                }
                else
                    return false;
            });

    hooks.erase(it, hooks.end());
}


void Core::addEffect(EffectHook *hook){
    if(!hook) return;

    hook->id = nextID++;

    if(hook->type == EFFECT_OVERLAY)
        effects.push_back(hook);
    else if(hook->type == EFFECT_WINDOW)
        hook->win->effects[hook->id] = hook;
}

void Core::remEffect(uint key, FireWindow w) {
    if(w == nullptr) {
        auto it = std::remove_if(effects.begin(), effects.end(),
                [key] (EffectHook *hook) {
            if(hook && hook->id == key) {
                hook->disable();
                return true;
            }
            else
                return false;
        });

        effects.erase(it, effects.end());
    }
    else {
        auto it = w->effects.find(key);
        it->second->disable();
        w->effects.erase(it);
    }
}

bool Hook::getState() { return this->active; }

void Hook::enable() {
    if(active)
        return;
    active = true;
    core->cntHooks++;
}

void Hook::disable() {
    if(!active)
        return;

    active = false;
    core->cntHooks--;
}

void KeyBinding::enable() {
    if(active) return;

    active = true;
    XGrabKey(core->d, key, mod, core->root,
            false, GrabModeAsync, GrabModeAsync);
}

void KeyBinding::disable() {
    if(!active) return;

    active = false;
    XUngrabKey(core->d, key, mod, core->root);
}

void Core::addKey(KeyBinding *kb, bool grab) {
    if(!kb) return;
    keys.push_back(kb);
    if(grab) kb->enable();
}

void Core::remKey(uint key) {
    auto it = std::remove_if(keys.begin(), keys.end(), [key] (KeyBinding *kb) {
                if(kb) return kb->id == key;
                else return true;
            });

    keys.erase(it, keys.end());
}

void ButtonBinding::enable() {
    if(active) return;

    active = true;
    XGrabButton(core->d, button, mod,
            core->root, false, ButtonPressMask,
            GrabModeAsync, GrabModeAsync,
            None, None);
}

void ButtonBinding::disable() {
    if(!active) return;

    active = false;
    XUngrabButton(core->d, button, mod, core->root);
}

void Core::addBut(ButtonBinding *bb, bool grab) {
    if(!bb) return;

    buttons.push_back(bb);
    if(grab) bb->enable();
}

void Core::remBut(uint key) {
    auto it = std::remove_if(buttons.begin(), buttons.end(), [key] (ButtonBinding *bb) {
                if(bb) return bb->id == key;
                else return true;
            });
    buttons.erase(it, buttons.end());
}

void Core::regOwner(Ownership owner) {
    owners.insert(owner);
}

bool Core::activateOwner(Ownership owner) {

    if(!owner) {
        std::cout << "Error detected ?? calling with nullptr!!1" << std::endl;
        return false;
    }

    if(owner->active || owner->special) {
        owner->active = true;
        return true;
    }

    for(auto act_owner : owners)
        if(act_owner && act_owner->active) {
            bool owner_in_act_owner_compat =
                act_owner->compat.find(owner->name) != act_owner->compat.end();

            bool act_owner_in_owner_compat =
                owner->compat.find(act_owner->name) != owner->compat.end();


            if(!owner_in_act_owner_compat && !act_owner->compatAll)
                return false;

            if(!act_owner_in_owner_compat && !owner->compatAll)
                return false;
        }

    owner->active = true;
    return true;
}

bool Core::deactivateOwner(Ownership owner) {
    owner->ungrab();
    owner->active = false;
    return true;
}

void Core::afterEffects() {
    std::vector<EffectHook*> runningEffects;
    for(auto effect : effects)
        if(effect && effect->getState())
            runningEffects.push_back(effect);

    for(auto effect : runningEffects)
        effect->action();

    glUseProgram(0);
    OpenGL::useDefaultProgram();
}

void Core::defaultRenderer() {
    XIntersectRegion(dmg, output, dmg);

    OpenGL::preStage();
    wins->renderWindows();
    afterEffects();
    OpenGL::endStage();

    if(resetDMG)
        XDestroyRegion(dmg),
        dmg = getRegionFromRect(0, 0, 0, 0),
        FireWin::allDamaged = false;
}

bool Core::setRenderer(RenderHook rh) {
    if(render.replaced)
        return false;
    render.replaced = true;
    render.currentRenderer = rh;
    return true;
}

void Core::setDefaultRenderer() {
    if(!render.replaced)
        return;

    render.replaced = false;

    render.currentRenderer =
        std::bind(std::mem_fn(&Core::defaultRenderer), this);
}

void Core::addSignal(std::string name) {
    if(signals.find(name) == signals.end())
        signals[name] = std::vector<SignalListener*>();
}

void Core::triggerSignal(std::string name, SignalListenerData data) {
    if(signals.find(name) != signals.end()) {
        std::vector<SignalListener*> toTrigger;
        for(auto proc : signals[name])
            toTrigger.push_back(proc);

        for(auto proc : signals[name])
            proc->action(data);
    }
}

void Core::connectSignal(std::string name, SignalListener *callback){
    addSignal(name);
    callback->id = nextID++;
    signals[name].push_back(callback);
}

void Core::disconnectSignal(std::string name, uint id) {
    auto it = std::remove_if(signals[name].begin(), signals[name].end(),
            [id](SignalListener *sigl){
            return sigl->id == id;
            });

    signals[name].erase(it, signals[name].end());
}

void Core::addDefaultSignals() {
    /* map-window and unmap-window are triggered
     * when a window is (un)mapped. The data they
     * contain is just a pointer to the FireWindow */
    addSignal("map-window");
    addSignal("unmap-window");

    /* move-window is triggered when a window is moved
     * Data contains 3 elements:
     * 1. raw pointer to FireWin
     * 2. dx
     * 3. dy */

    addSignal("move-window");
}

void Core::addWindow(XCreateWindowEvent xev) {
    FireWindow w = std::make_shared<FireWin>(xev.window, true);

    if(xev.parent != root && xev.parent != 0 && !w->transientFor)
        w->transientFor = findWindow(xev.parent);

    wins->addWindow(w);
    if(w->type != WindowTypeWidget)
        wins->focusWindow(w);
}

void Core::addWindow(Window id) {
    XCreateWindowEvent xev;
    xev.window = id;
    xev.parent = 0;
    addWindow(xev);
}

void Core::focusWindow(FireWindow win) {
    wins->focusWindow(win);
}

FireWindow Core::findWindow(Window win) {
    return wins->findWindow(win);
}

FireWindow Core::getActiveWindow() {
    return wins->getTopmostToplevel();
}

void Core::mapWindow(FireWindow win, bool xmap) {
    if(xmap)
        XMapWindow(d, win->id),
        XSync(d, 0);

    if(win->initialMapping){
        win->initialMapping = false;
        /* ensure windows do not save their position to some
         * other workspace which might not exist */
        int x = win->attrib.x, y = win->attrib.y;
        if(WinUtil::constrainNewWindowPosition(x, y))
            win->move(x, y, true);
    }

    win->syncAttrib();
    win->addDamage();

    if(win->attrib.map_state == IsViewable)
        win->pixmap = XCompositeNameWindowPixmap(d, win->id);
    else
        win->pixmap = 0;

    if(win->transientFor)
        wins->restackTransients(win->transientFor);
    wins->checkAddClient(win);

    SignalListenerData v;
    v.push_back((void*)&win);
    triggerSignal("map-window", v);
}

void Core::unmapWindow(FireWindow win) {
    win->attrib.map_state = IsUnmapped;

    SignalListenerData v;
    v.push_back((void*)&win);
    triggerSignal("unmap-window", v);

    wins->checkRemoveClient(win);
}

void Core::closeWindow(FireWindow win) {
    if(!win) return;

    int cnt;
    Atom *atoms;
    auto status = XGetWMProtocols(d, win->id, &atoms, &cnt);
    Atom wmdelete = XInternAtom(d, "WM_DELETE_WINDOW", 0);
    Atom wmproto  = XInternAtom(d, "WM_PROTOCOLS", 0);

    if(status != 0) {
        bool send = false; // should we send a wm_delete_window?
        for(int i = 0; i < cnt; i++)
            if(atoms[i] == wmdelete)
                send = true;

        if(send) {
            XEvent xev;

            xev.type         = ClientMessage;
            xev.xclient.window       = win->id;
            xev.xclient.message_type = wmproto;
            xev.xclient.format       = 32;
            xev.xclient.data.l[0]    = wmdelete;
            xev.xclient.data.l[1]    = CurrentTime;
            xev.xclient.data.l[2]    = 0;
            xev.xclient.data.l[3]    = 0;
            xev.xclient.data.l[4]    = 0;

            XSendEvent (d, win->id, FALSE, NoEventMask, &xev);
        } else
            XKillClient (d, win->id);
    } else
        XKillClient(d, win->id);

    wins->focusWindow(wins->getTopmostToplevel());
}

void Core::removeWindow(FireWindow win) {
    wins->removeWindow(win);
    win.reset();
}

void Core::wait(int timeout) {
    poll(&fd, 1, timeout / 1000); // convert from usec to msec
}

bool Core::checkKey(KeyBinding *kb, XKeyEvent xkey) {
    if(!kb->active)
        return false;

    if(kb->key != xkey.keycode)
        return false;

    if(kb->mod != xkey.state)
        return false;

    return true;
}

bool Core::checkButPress(ButtonBinding *bb, XButtonEvent xb) {

    if(!bb->active)
        return false;

    if(bb->type != BindingTypePress)
        return false;

    auto state = xb.state & AllModifiers;
    if(state != bb->mod && bb->mod != AnyModifier)
        return false;

    if(bb->button != xb.button)
        return false;
    return true;
}

bool Core::checkButRelease(ButtonBinding *bb, XButtonEvent kb) {
    if(!bb->active)
        return false;

    if(bb->type != BindingTypeRelease)
        return false;

    if(bb->button != kb.button)
        return false;

    return true;
}

void Core::handleEvent(XEvent xev){
    switch(xev.type) {
        case Expose:
            dmg = getMaximisedRegion();
            break;
        case KeyPress: {
            // check keybindings
            for(auto kb : keys)
                if(checkKey(kb, xev.xkey))
                   kb->action(new Context(xev));
            break;
        }

        case CreateNotify: {
                        if (xev.xcreatewindow.window == overlay)
                break;

            if(xev.xcreatewindow.window == outputwin)
                break;

            auto it = findWindow(xev.xcreatewindow.window);

            /* guard against (almost) indiscoverable bugs,
             * i.e we kept some destroyed window */
            if(it != nullptr) {
                std::cout << "old window!!!" << std::endl;
                wins->removeWindow(it);
            }

            addWindow(xev.xcreatewindow);
            break;
        }
        case DestroyNotify: {
            auto w = wins->findWindow(xev.xdestroywindow.window);
            if(!w) break;
            w->destroyed = true;
            if(!w->keepCount)
                removeWindow(w);
            break;
        }
        case MapRequest: {
            auto w = wins->findWindow(xev.xmaprequest.window);
            if(w) mapWindow(w);
            break;
        }
        case MapNotify: {
            auto w = findWindow(xev.xmap.window);
            if(w) mapWindow(w, false),
                  wins->focusWindow(w);
            break;
        }
        case UnmapNotify: {
            auto w = wins->findWindow(xev.xunmap.window);
            if(w) unmapWindow(w);
            break;
        }

        case ButtonPress: {
            mousex = xev.xbutton.x_root;
            mousey = xev.xbutton.y_root;

            for(auto bb : buttons) {
                if(checkButPress(bb, xev.xbutton)) {
                    bb->action(new Context(xev));
                    break;
                }
            }

            XAllowEvents(d, ReplayPointer, xev.xbutton.time);
            break;
        }
        case ButtonRelease:
            for(auto bb : this->buttons)
                if(checkButRelease(bb, xev.xbutton))
                    bb->action(new Context(xev));

            XAllowEvents(d, ReplayPointer, xev.xbutton.time);
            break;

        case MotionNotify:
            mousex = xev.xmotion.x_root;
            mousey = xev.xmotion.y_root;
            break;

        case ConfigureRequest: {
            auto w = findWindow(xev.xconfigurerequest.window);
            if(!w) { // from compiz window manager
                XWindowChanges xwc;
                std::memset(&xwc, 0, sizeof(xwc));

                auto xwcm = xev.xconfigurerequest.value_mask &
                    (CWX | CWY | CWWidth | CWHeight | CWBorderWidth);

                xwc.x        = xev.xconfigurerequest.x;
                xwc.y        = xev.xconfigurerequest.y;
                xwc.width        = xev.xconfigurerequest.width;
                xwc.height       = xev.xconfigurerequest.height;
                xwc.border_width = xev.xconfigurerequest.border_width;

                XConfigureWindow (d, xev.xconfigurerequest.window,
                        xwcm, &xwc);
                break;
            }

            int width = w->attrib.width, height = w->attrib.height;
            int x = w->attrib.x, y = w->attrib.y;

            if(xev.xconfigurerequest.value_mask & CWWidth)
                width = xev.xconfigurerequest.width;
            if(xev.xconfigurerequest.value_mask & CWX)
                x = xev.xconfigurerequest.x;
            if(xev.xconfigurerequest.value_mask & CWHeight)
                height = xev.xconfigurerequest.height;
            if(xev.xconfigurerequest.value_mask & CWY)
                y = xev.xconfigurerequest.y;

            w->moveResize(x, y, width, height);

            /* note : pass XConfigureRequest to server, then wait for ConfigureNotify */

            if(xev.xconfigurerequest.value_mask & CWStackMode &&
               xev.xconfigurerequest.value_mask & CWSibling) {
                XWindowChanges xwc;
                xwc.sibling = xev.xconfigurerequest.above;
                xwc.stack_mode = xev.xconfigurerequest.detail;
                auto mask = CWStackMode | CWSibling;

                XConfigureWindow(d, w->id, mask, &xwc);
            }

//            if(xev.xconfigurerequest.value_mask & CWStackMode) {
//                if(xev.xconfigurerequest.above) {
//                    auto below = findWindow(xev.xconfigurerequest.above);
//                    if(below) {
//                        std::cout << "Calling from ConfigureRequest" << std::endl;
//                        if(xev.xconfigurerequest.detail == Above)
//                            wins->restackAbove(w, below);
//                        else
//                            wins->restackAbove(below, w);
//                    }
//                }
//                else {
//                    if(xev.xconfigurerequest.detail == Above) {
//                        focusWindow(w);
//                    }
//                }
//            }
            mapWindow(w, false);

        }

        case PropertyNotify: {
            auto w = findWindow(xev.xproperty.window);
            if(!w) break;

            if(xev.xproperty.atom == winTypeAtom) {
                w->type = WinUtil::getWindowType(w->id);
                wins->recalcWindowLayer(w);
                wins->restackTransients(w);
            }

            if(xev.xproperty.atom == winStateAtom) {
                w->state = WinUtil::getWindowState(w->id);
                w->updateState();
                wins->recalcWindowLayer(w);
                wins->restackTransients(w);
            }

            if(xev.xproperty.atom == XA_WM_TRANSIENT_FOR)
                w->transientFor = WinUtil::getTransient(w->id),
                wins->restackTransients(w);

            if(xev.xproperty.atom == wmClientLeaderAtom)
                w->leader = WinUtil::getClientLeader(w->id),
                wins->restackTransients(w);
            break;
        }

        case ConfigureNotify: {
            if(xev.xconfigure.window == root) {
                terminate = true, mainrestart = true;
                break;
            }

            auto w = findWindow(xev.xconfigure.window);
            if(!w) break;

            if(xev.xconfigure.width != w->attrib.width ||
                    xev.xconfigure.height != w->attrib.height)
                w->resize(xev.xconfigure.width, xev.xconfigure.height, false);

            if(xev.xconfigure.x != w->attrib.x ||
                    xev.xconfigure.y != w->attrib.y)
                w->move(xev.xconfigure.x, xev.xconfigure.y, false);

            wins->restackAbove(w, findWindow(xev.xconfigure.above));
            break;
        }

        case ClientMessage: {
            if (xev.xclient.message_type == activeWinAtom) {
                auto w = findWindow(xev.xclient.window);
                if(!w) break;
                wins->focusWindow(w);
            }
            break;
        }

        case EnterNotify:
        case LeaveNotify:       // we ignore
        case FocusIn:           // all of these
        case MappingNotify:
        case SelectionRequest:
        case SelectionNotify:
        case KeyRelease:
        case CirculateRequest:
        case CirculateNotify:
        case ReparentNotify:
            break;

        default:
            if(xev.type == damage + XDamageNotify) {
                if(FireWin::allDamaged)
                    break;

                XDamageNotifyEvent *x =
                    reinterpret_cast<XDamageNotifyEvent*> (&xev);

                auto w = findWindow(x->drawable);
                if(!w) break;

                w->damaged = true;

                if(!w->visible)
                    break;

                auto damagedArea = getREGIONFromRect(
                        x->area.x + w->attrib.x,
                        x->area.y + w->attrib.y,
                        x->area.x + w->attrib.x + x->area.width,
                        x->area.y + w->attrib.y + x->area.height);

                damageREGION(damagedArea);
            break;
            }
    }
}
#define Second 1000000
#define MaxDelay 1000
#define MinRR 61

void Core::loop(){

    if(nextID == (uint)(-1)) {
        mainrestart = true, terminate = true;
        return;
    }

    int currentCycle = Second / plug->options["rrate"]->data.ival;
    int baseCycle = currentCycle;

    timeval before, after;
    gettimeofday(&before, 0);
    bool hadEvents = false;

    XEvent xev;
    while(!terminate) {
        /* handle current events */
        while(XPending(d)) {
            XNextEvent(d, &xev);
            handleEvent(xev);
        }

        gettimeofday(&after, 0);
        int diff = (after.tv_sec - before.tv_sec) * 1000000 +
            after.tv_usec - before.tv_usec;

        if(diff < currentCycle) {     // we have time to next redraw, wait
            wait(currentCycle - diff);// for events
            if(fd.revents & POLLIN || !resetDMG || cntHooks) {
                /* disable optimisation */
                hadEvents = true;
                currentCycle = baseCycle;
                continue;
            }
            fd.revents = 0;

        }
        else {

            if(cntHooks) {
                /* copy running hooks,
                 * since a hook can remove itself
                 * causing a crash(same pattern is used
                 * in other places such as effects, too) */
                std::vector<Hook*> runningHooks;
                for (auto hook : hooks)
                    if(hook->getState())
                        runningHooks.push_back(hook);

                for(auto hook : runningHooks)
                    hook->action();
            }

            /* if some screen region is damaged, draw it */
            if(FireWin::allDamaged || !XEmptyRegion(dmg))
                render.currentRenderer();

            /* optimisation when idle */
            if(!cntHooks && !hadEvents && resetDMG)
                currentCycle *= 2;

            before = after;
            hadEvents = false;
        }
    }
}

int Core::onOtherWmDetected(Display* d, XErrorEvent *xev) {
    if(static_cast<int>(xev->error_code) == BadAccess)
        wmDetected = true;
    return 0;
}

int Core::onXError(Display *d, XErrorEvent *xev) {
    if(xev->resourceid == 0) // invalid window
        return 0;

    std::cout << std::endl << "____________________________" << std::endl;
    std::cout << "XError code   " << int(xev->error_code) << std::endl;
    char buf[512];
    XGetErrorText(d, xev->error_code, buf, 512);
    std::cout << "XError string " << buf << std::endl;
    std::cout << "ResourceID = " << xev->resourceid << std::endl;
    std::cout << "____________________________" << std::endl << std::endl;

    //print_trace();
    return 0;
}

std::tuple<int, int> Core::getWorkspace() {
    return std::make_tuple(vx, vy);
}

std::tuple<int, int> Core::getWorksize() {
    return std::make_tuple(vwidth, vheight);
}

std::tuple<int, int> Core::getScreenSize() {
    return std::make_tuple(width, height);
}

std::tuple<int, int> Core::getMouseCoord() {
    return std::make_tuple(mousex, mousey);
}

void Core::switchWorkspace(std::tuple<int, int> nPos) {
    auto nx = std::get<0> (nPos);
    auto ny = std::get<1> (nPos);

    if(nx >= vwidth || ny >= vheight || nx < 0 || ny < 0)
        return;

    auto dx = (vx - nx) * width;
    auto dy = (vy - ny) * height;
    GetTuple(sw, sh, core->getScreenSize());

    using namespace std::placeholders;
    auto proc = [dx, dy, sw, sh] (FireWindow w) {
        //if(w->state & WindowStateSticky)
        //    return;
        w->move(w->attrib.x + dx, w->attrib.y + dy);

        if(w->attrib.x > sw || w->attrib.y > sh ||
                w->attrib.x + w->attrib.width < 0 ||
                w->attrib.y + w->attrib.height < 0)
            w->visible = false;
        else
            w->visible = true;
    };
    wins->forEachWindow(proc);

    vx = nx;
    vy = ny;

    auto ws = getWindowsOnViewport(this->getWorkspace());
    if(ws.size() != 0)
        wins->focusWindow(ws[0]);
}

std::vector<FireWindow> Core::getWindowsOnViewport(std::tuple<int, int> vp) {
    auto x = std::get<0>(vp);
    auto y = std::get<1>(vp);

    auto view = getRegionFromRect((x - vx) * width, (y - vy) * height,
                       (x - vx + 1) * width, (y - vy + 1) * height);

    std::vector<FireWindow> ret;
    Region tmp = XCreateRegion();

    auto proc = [view, &ret, tmp] (FireWindow w) {
        if(!w->region)
            return;

        if(w->state & WindowStateSkipTaskbar)
            return;

        if(w->type == WindowTypeWidget ||
                w->type == WindowTypeDock)
            return;

        XIntersectRegion(view, w->region, tmp);
        if(tmp && !XEmptyRegion(tmp) && !w->norender)
            ret.push_back(w);
    };

    wins->forEachWindow(proc);
    XDestroyRegion(view);
    XDestroyRegion(tmp);

    return ret;
}

void Core::getViewportTexture(std::tuple<int, int> vp,
        GLuint &fbuff, GLuint &texture) {

    OpenGL::useDefaultProgram();
    if(fbuff == -1 || texture == -1)
        OpenGL::prepareFramebuffer(fbuff, texture);
    OpenGL::preStage(fbuff);
    glClear(GL_COLOR_BUFFER_BIT);

    GetTuple(x, y, vp);
    FireWin::allDamaged = true;

    auto view = getRegionFromRect((x - vx) * width, (y - vy) * height,
                       (x - vx + 1) * width, (y - vy + 1) * height);

    glm::mat4 off = glm::translate(glm::mat4(),
            glm::vec3(2 * (vx - x), 2 * (y - vy), 0));
    glm::mat4 save = Transform::gtrs;
    Transform::gtrs *= off;

    Region tmp = XCreateRegion();
    std::vector<FireWindow> winsToDraw;

    auto proc = [view, tmp, &winsToDraw](FireWindow win) {
        if(!win->isVisible() || !win->region)
            return;

        XIntersectRegion(view, win->region, tmp);
        if(!XEmptyRegion(tmp))
            winsToDraw.push_back(win);
    };
    wins->forEachWindow(proc);

    auto it = winsToDraw.rbegin();
    while(it != winsToDraw.rend())
        (*it++)->render();

    Transform::gtrs = save;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

namespace {
    int fullRedraw = 0;
}

void Core::setRedrawEverything(bool val) {
    if(val) {
        fullRedraw++;

        output = getRegionFromRect(-vx * width, -vy * height,
                (vwidth  - vx) * width, (vheight - vy) * height);
        FireWin::allDamaged = true;
        core->resetDMG = false;
    }
    else if(--fullRedraw == 0)
        output = getMaximisedRegion(),
        FireWin::allDamaged = false,
        core->resetDMG = true;
}

REGION Core::getREGIONFromRect(int tlx, int tly, int brx, int bry) {
    REGION r;
    r.numRects = r.size = 1;
    r.rects = new BOX[1];
    r.extents.x1 = r.rects[0].x1 = tlx;
    r.extents.y1 = r.rects[0].y1 = tly;
    r.extents.x2 = r.rects[0].x2 = brx;
    r.extents.y2 = r.rects[0].y2 = bry;
    return r;
}

Region Core::getRegionFromRect(int tlx, int tly, int brx, int bry) {
    auto r = getREGIONFromRect(tlx, tly, brx, bry);
    Region ret;
    ret = XCreateRegion();
    XUnionRegion(&r, ret, ret);
    return ret;
}

Region Core::getMaximisedRegion() {
    return getRegionFromRect(0, 0, width, height);
}

#define MAX_DAMAGED_RECTS 150

void Core::damageRegion(Region r) {
    XUnionRegion(r, dmg, dmg);
    if(dmg->numRects > MAX_DAMAGED_RECTS)
        FireWin::allDamaged = true;
}

void Core::damageREGION(REGION r) {
    damageRegion(&r);
}

int Core::getRefreshRate() {
    return refreshrate;
}

#define uchar unsigned char
namespace {
    GLuint getFilledTexture(int w, int h, uchar r, uchar g, uchar b, uchar a) {
        int *arr = new int[w * h * 4];
        for(int i = 0; i < w * h; i = i + 4) {
            arr[i + 0] = r;
            arr[i + 1] = g;
            arr[i + 2] = b;
            arr[i + 3] = a;
        }
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA,
                w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, arr);

        glBindTexture(GL_TEXTURE_2D, 0);
        return tex;
    }
}

void Core::setBackground(const char *path) {
    std::cout << "[DD] Background file: " << path << std::endl;

    auto texture = GLXUtils::loadImage(const_cast<char*>(path));

    if(texture == -1)
        texture = getFilledTexture(width, height, 128, 128, 128, 255);

    uint vao, vbo;
    OpenGL::generateVAOVBO(0, height, width, -height, vao, vbo);

    backgrounds.clear();
    backgrounds.resize(vheight);
    for(int i = 0; i < vheight; i++)
        backgrounds[i].resize(vwidth);

    for(int i = 0; i < vheight; i++){
        for(int j = 0; j < vwidth; j++) {

            backgrounds[i][j] = std::make_shared<FireWin>(0, false);

            backgrounds[i][j]->vao = vao;
            backgrounds[i][j]->vbo = vbo;
            backgrounds[i][j]->norender = false;
            backgrounds[i][j]->texture  = texture;
            backgrounds[i][j]->attrib.map_state = InputOutput;

            backgrounds[i][j]->attrib.x = (j - vx) * width;
            backgrounds[i][j]->attrib.y = (i - vy) * height;
            backgrounds[i][j]->attrib.width  = width;
            backgrounds[i][j]->attrib.height = height;

            backgrounds[i][j]->type = WindowTypeDesktop;
            backgrounds[i][j]->updateVBO();
            backgrounds[i][j]->transform.color = glm::vec4(1,1,1,1);
            backgrounds[i][j]->updateRegion();
            wins->addWindow(backgrounds[i][j]);
        }
    }
}

namespace {
    template<class A, class B> B unionCast(A object) {
        union {
            A x;
            B y;
        } helper;
        helper.x = object;
        return helper.y;
    }
}

PluginPtr Core::loadPluginFromFile(std::string path, void **h) {
    void *handle = dlopen(path.c_str(), RTLD_NOW);
    if(handle == NULL){
        std::cout << "Error loading plugin " << path << std::endl;
        std::cout << dlerror() << std::endl;
        return nullptr;
    }

    auto initptr = dlsym(handle, "newInstance");
    if(initptr == NULL) {
        std::cout << "Failed to load newInstance from file " <<
            path << std::endl;
        std::cout << dlerror();
        return nullptr;
    }
    LoadFunction init = unionCast<void*, LoadFunction>(initptr);
    *h = handle;
    return std::shared_ptr<Plugin>(init());
}

void Core::loadDynamicPlugins() {
    std::stringstream stream(*plug->options["plugins"]->data.sval);
    auto path = *plug->options["pluginpath"]->data.sval+"/fireman/";

    std::string plugin;
    while(stream >> plugin){
        if(plugin != "") {
            void *handle;
            auto ptr = loadPluginFromFile(path + "/lib" + plugin + ".so",
                    &handle);
            if(ptr) ptr->handle  = handle,
                    ptr->dynamic = true,
                    plugins.push_back(ptr);
        }
    }
}

template<class T>
PluginPtr Core::createPlugin() {
    return std::static_pointer_cast<Plugin>(std::make_shared<T>());
}

void Core::initDefaultPlugins() {
    plug = createPlugin<CorePlugin>();
    plugins.push_back(createPlugin<Focus>());
    plugins.push_back(createPlugin<Exit>());
    plugins.push_back(createPlugin<Close>());
    plugins.push_back(createPlugin<Refresh>());
}
