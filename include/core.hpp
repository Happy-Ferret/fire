#ifndef FIRE_H
#define FIRE_H

#include "commonincludes.hpp"
#include "window.hpp"
#include "glx.hpp"
#include <queue>


extern bool inRenderWindow;
class WinStack;

struct Context{
    XEvent xev;
    Context(XEvent xev);
};


enum BindingType {
    BindingTypePress,
    BindingTypeRelease
};

struct Binding{
    bool active;
    BindingType type;
    uint mod;

    std::function<void(Context*)> action;

};

// keybinding
struct KeyBinding : Binding {
    KeyCode key;
};

// button binding

struct ButtonBinding : Binding {
    uint button;
};

// hooks are done once a redraw cycle
struct Hook {
    private:
        bool active;
    public:
        std::function<void(void)> action;
        void enable();
        void disable();
        bool getState();
        Hook();
};

class Focus;
class WindowOperation {
    protected:
        int sx, sy; // starting pointer x, y
        FireWindow win; // window we're operating on
        uint hid; // id of hook
    protected:
        ButtonBinding press;
        ButtonBinding release;
        Hook hook;
};

class Move : WindowOperation {
    public:
        Move(Core *core);
        void Initiate(Context*);
        void Intermediate();
        void Terminate(Context*);
};

class Resize : WindowOperation {
    public:
        Resize(Core *core);
        void Initiate(Context*);
        void Intermediate();
        void Terminate(Context*);
};

class WSSwitch {
    private:
        KeyBinding kbs[4];
        Hook hook;
        int stepNum;
        int dirx, diry;
        int dx, dy;
        int nx, ny;
        std::queue<std::tuple<int, int> >dirs; // series of moves we have to do
        void beginSwitch();
    public:
        WSSwitch(Core *core);
        void moveWorkspace(int dx, int dy);
        void handleSwitchWorkspace(Context *ctx);
        void moveStep();
};

class Expo {
    private:
        KeyBinding keys[4];
        KeyBinding toggle;
        ButtonBinding press, release;

        int stepNum;

        float offXtarget, offYtarget;
        float offXcurrent, offYcurrent;
        float sclXtarget, sclYtarget;
        float sclXcurrent, sclYcurrent;

        float stepoffX, stepoffY, stepsclX, stepsclY;

        Hook hook;
        bool active;
        std::function<FireWindow(Point)> save; // used to restore
    public:                                // WinStack::find...Position
        Expo(Core *core);
        void handleKey(Context *ctx);
        void Toggle(Context *ctx);
        FireWindow findWindow(Point p);
        void buttonRelease(Context *ctx);
        void buttonPress(Context *ctx);
        void recalc();
        void zoom();
        void finalizeZoom();
};

class ATSwitcher {
    KeyBinding initiate;
    KeyBinding forward;
    KeyBinding backward;
    KeyBinding terminate;
    std::vector<FireWindow> windows;
    FireWindow background;
    bool active;
    int index;
    Hook rnd; // used to render windows
    public:
        ATSwitcher(Core *core);
        void handleKey(Context *ctx);
        void moveLeft();
        void moveRight();
        void Initiate();
        void Terminate();
        void render();
        void reset();
        float getFactor(int x, int y, float percent);
};

class Grid{

    struct GridWindow {
        Window id;
        XWindowAttributes size;
        GridWindow(Window id, int x, int y, int w, int h);
    };

    std::vector<GridWindow> wins;
    KeyBinding keys[10];
    KeyCode codes[10];

    public:
        Grid(Core *core);
        void handleKey(Context*);
        void toggleMaxim(FireWindow win);
        void getSlot(int n, int &x, int &y, int &w, int &h);
};

class Close;

class Core {
    friend class Focus;
    friend class Move;
    friend class Resize;
    friend class WSSwitch;
    friend class Expo;
    friend class Close;
    friend class ATSwitcher;
    friend class Grid;

    private:
        std::vector<std::vector<FireWindow> > backgrounds;
        int damage;

        static WinStack *wins;
        void handleEvent(XEvent xev);
        void wait(int timeout);
        void enableInputPass(Window win);

        pollfd fd;

        int mousex, mousey; // pointer x, y

        int vwidth, vheight; // viewport size
        int vx, vy;          // viewport position

        std::unordered_map<uint, KeyBinding*> keys;
        std::unordered_map<uint, ButtonBinding*> buttons;
        std::unordered_map<uint, Hook*> hooks;

        template<class T>
        uint getFreeID(std::unordered_map<uint, T> *map);

        KeyCode switchWorkspaceBindings[4];

        Focus *focus;
        Exit *exit;
        Run *runn;
        Expo *expo;
        WSSwitch *wsswitch;
        Move *move;
        Resize *resize;
        Close *close;
        ATSwitcher *at;
        Grid *grid;

    public:
        Display *d;
        Window root;
        Window overlay;
        Window outputwin;

        int cntHooks;

        int width;
        int height;

        bool redraw = true; // should we redraw?

        float scaleX = 1, scaleY = 1; // used for operations which
                              // depend on mouse moving
                              // for ex. when using expo
        void setBackground(const char *path);

        void switchWorkspace(std::tuple<int, int>);
        std::tuple<int, int> getWorkspace();

        uint addKey(KeyBinding *kb, bool grab = false);
        void remKey(uint id);

        uint addBut(ButtonBinding *bb, bool grab = false);
        void remBut(uint id);

        uint addHook(Hook*);
        void remHook(uint);

    public:
        ~Core();
        void loop();
        Core();

        void run(char *command);

        void renderAllWindows();
        void addWindow(XCreateWindowEvent);
        FireWindow findWindow(Window win);
        FireWindow getActiveWindow();
        void destroyWindow(FireWindow win);
        std::vector<FireWindow> getWindowsOnViewport(int x, int y);

        static int onXError (Display* d, XErrorEvent* xev);
        static int onOtherWmDetected(Display *d, XErrorEvent *xev);
};
extern Core *core;

#endif
