#include "../include/core.hpp"
#include <X11/extensions/Xrender.h>

namespace GLXUtils {

    namespace {
        typedef GLXContext (*glXCreateContextAttribsARBProc) (Display*,
                GLXFBConfig, GLXContext,
                Bool, const int* );

        PFNGLXBINDTEXIMAGEEXTPROC glXBindTexImageEXT_func = NULL;
        PFNGLXRELEASETEXIMAGEEXTPROC glXReleaseTexImageEXT_func = NULL;

        bool useXShm = false;
        XVisualInfo *defaultVisual;
    }


GLXFBConfig fbconfigs[33];

GLuint loadImage (char* path) {
    GLuint textureID;
    ILuint imageID;

    ilGenImages ( 1, &imageID );
    ilBindImage ( imageID );
    if ( !ilLoadImage ( path ) ) {
        err << "Can't open image " << path;
        std::exit ( 1 );
    }

    ILinfo infoi;
    iluGetImageInfo ( &infoi );
    if ( infoi.Origin == IL_ORIGIN_UPPER_LEFT )
        iluFlipImage();

    if ( !ilConvertImage (IL_BGR, IL_UNSIGNED_BYTE))
        err << "Can't convert image!", std::exit ( 1 );

    glGenTextures ( 1, &textureID );
    printf ( "texture id : %d \n", textureID );
    glBindTexture ( GL_TEXTURE_2D, textureID );

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    glTexImage2D ( GL_TEXTURE_2D, 0,
            GL_RGBA,
            ilGetInteger (IL_IMAGE_WIDTH),
            ilGetInteger (IL_IMAGE_HEIGHT),
            0,
//            ilGetInteger(IL_IMAGE_FORMAT)
            GL_RGB,
            GL_UNSIGNED_BYTE,
            ilGetData() );
    return textureID;
}


void createNewContext(Window win) {
    int dummy;
    GLXFBConfig *fbconfig = glXGetFBConfigs(core->d,
            DefaultScreen(core->d), &dummy);

    if ( fbconfig == NULL )
        printf ( "Couldn't get FBConfigs list!\n" ),
               std::exit (-1);

    auto xvi = WinUtil::getVisualInfoForWindow(win);
    int i = 0;
    for (; i < dummy; i++ ) {
        auto id = glXGetVisualFromFBConfig(core->d, fbconfig[i]);
        if ( xvi->visualid == id->visualid )
            break;
    }

    if ( i == dummy ) {
        err << "failed to get FBConfig for opengl 3+ context!";
        std::exit(-1);
    }


    int ogl33[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
        GLX_CONTEXT_MINOR_VERSION_ARB, 4,
        GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, None
    };
    glXCreateContextAttribsARBProc glXCreateContextAttribsARB =
        (glXCreateContextAttribsARBProc) glXGetProcAddressARB(
                (const GLubyte *) "glXCreateContextAttribsARB" );
    GLXContext context = glXCreateContextAttribsARB (core->d,
            fbconfig[i], NULL, true, ogl33 );

    glXMakeCurrent ( core->d, win, context );

}
void createDefaultContext(Window win) {
    auto xvi = WinUtil::getVisualInfoForWindow(win);
    GLXContext ctx = glXCreateContext(core->d, xvi, NULL, 0);
    glXMakeCurrent(core->d, win, ctx);
    XFree(xvi);
}


static int attrListDbl[] = {
     GLX_DOUBLEBUFFER, True,
     GLX_RED_SIZE, 8,
     GLX_GREEN_SIZE, 8,
     GLX_BLUE_SIZE, 8,
     GLX_DEPTH_SIZE, 8,
     GLX_STENCIL_SIZE, 8,
     GLX_ALPHA_SIZE, 8,
     None
 };
static int attrListVisual[] = {
     GLX_RGBA, GLX_DOUBLEBUFFER,
     GLX_RED_SIZE, 8,
     GLX_BLUE_SIZE, 8,
     GLX_GREEN_SIZE, 8,
     GLX_DEPTH_SIZE, 8,
     GLX_STENCIL_SIZE, 8,
     GLX_ALPHA_SIZE, 8,
     None
 };

Window createNewWindowWithContext(Window parent, Core *core) {
    Colormap cmap;
    XSetWindowAttributes winAttr;

    defaultVisual = glXChooseVisual ( core->d,
            DefaultScreen(core->d), attrListVisual );
    if ( defaultVisual == NULL )
        err << "Couldn't get visual!", std::exit (1);

    cmap = XCreateColormap ( core->d, core->root,
            defaultVisual->visual, AllocNone );
    winAttr.colormap = cmap;
    winAttr.border_pixel = 0;
    winAttr.event_mask = ExposureMask;

    auto t = core->getScreenSize();
    auto w = std::get<0>(t);
    auto h = std::get<1>(t);

    auto window = XCreateWindow ( core->d,core->root,
            0, 0, w, h, 0,
            defaultVisual->depth, InputOutput, defaultVisual->visual,
            CWBorderPixel | CWColormap | CWEventMask,
            &winAttr );


    XReparentWindow(core->d, window, core->overlay, 0, 0);
    XMapRaised ( core->d, window );

    int dummy;
    GLXFBConfig *fbconfig = glXChooseFBConfig (core->d,
            DefaultScreen(core->d), attrListDbl, &dummy );

    if ( fbconfig == NULL )
        err << "Couldn't get FBConfig!", std::exit (1);

    int ogl44[]= {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
        GLX_CONTEXT_MINOR_VERSION_ARB, 4,
        GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, None
    };

    glXCreateContextAttribsARBProc glXCreateContextAttribsARB =
        (glXCreateContextAttribsARBProc) glXGetProcAddressARB(
                (const GLubyte *) "glXCreateContextAttribsARB" );

    GLXContext context = glXCreateContextAttribsARB ( core->d,
            fbconfig[0], NULL, true, ogl44 );

    glXMakeCurrent ( core->d, window, context );
    return window;
}


#define uchar unsigned char


void initGLX(Core *core) {

    auto x = glXGetCurrentContext();
    if ( x == NULL )
        err << "current context is NULL!!!" << std::endl,
            std::exit(-1);

    glewExperimental = GL_TRUE;
    auto res = glewInit();
    glGetError(); // workaround for glew getting Invalid Enum
    if ( res != GLEW_OK ) {
        err << "failed to init glew" << std::endl;
        std::exit(-1);
    }

    ilInit();
    iluInit();
    ilutInit();
    ilutRenderer ( ILUT_OPENGL );

    glXBindTexImageEXT_func = (PFNGLXBINDTEXIMAGEEXTPROC)
        glXGetProcAddress((GLubyte *) "glXBindTexImageEXT");
    glXReleaseTexImageEXT_func = (PFNGLXRELEASETEXIMAGEEXTPROC)
        glXGetProcAddress((GLubyte*) "glXReleaseTexImageEXT");

    int ignore, major, minor;
    Bool pixmaps;

    /* Check for the XShm extension */
    if( XQueryExtension(core->d, "MIT-SHM", &ignore, &ignore, &ignore))
        if(XShmQueryVersion(core->d, &major, &minor, &pixmaps) == True)
            useXShm = true;
}

GLuint compileShader(const char *src, GLuint type) {
    GLuint shader = glCreateShader(type);
    glShaderSource ( shader, 1, &src, NULL );

    int s;
    char b1[512];
    glCompileShader ( shader );
    glGetShaderiv ( shader, GL_COMPILE_STATUS, &s );
    glGetShaderInfoLog ( shader, 512, NULL, b1 );


    if ( s == GL_FALSE ) {
        std::stringstream srcStream, errorStream;
        std::string line;
        err << "shader compilation failed!";
        err << "src: *****************************";
        srcStream << src;
        while(std::getline(srcStream, line))
            err << line;
        err << "**********************************";
        errorStream << b1;
        while(std::getline(errorStream, line))
            err << line;
        err << "**********************************";
        return -1;
    }
    return shader;
}

GLuint loadShader(const char *path, GLuint type) {

    std::fstream file(path);
    if(!file.is_open())
        err << "Cannot open shader file. Aborting", std::exit(1);

    std::string str, line;

    while(std::getline(file, line))
        str += line, str += '\n';

    return compileShader(str.c_str(), type);
}


void endFrame(Window win) {
    glXSwapBuffers(core->d, win);
}

GLuint textureFromWindow(Window id, int w, int h, SharedImage *sim) {
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    auto pix = XCompositeNameWindowPixmap(core->d, id);
    if(useXShm) {

        if(sim->init) {

            sim->image = XShmCreateImage(core->d, defaultVisual->visual,
                    defaultVisual->depth, ZPixmap, NULL, &sim->shminfo, w, h);

            if(sim->image == NULL) return -1;

            /* Get the shared memory and check for errors */
            sim->shminfo.shmid = shmget(IPC_PRIVATE,
                    sim->image->bytes_per_line * sim->image->height,
                    IPC_CREAT | 0777);

            if(sim->shminfo.shmid < 0) return -1;

            sim->shminfo.shmaddr = sim->image->data =
                (char *)shmat(sim->shminfo.shmid, 0, 0);
            if(sim->shminfo.shmaddr == (char *) -1) return -1;

            /* set as read/write, and attach to the display */
            sim->shminfo.readOnly = False;
            sim->init = false;
            sim->existing = true;
            XShmAttach(core->d, &sim->shminfo);
        }

        XShmGetImage(core->d, pix, sim->image, 0, 0, AllPlanes);

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, (void*)(&sim->image->data[0]));
    }
    else { /* we cannot use XShm extension, so fallback to
              the *slower* XGetImage */
        auto xim = XGetImage(core->d, pix, 0, 0, w, h, AllPlanes, ZPixmap);
        if(xim == nullptr){
            std::cout << "xgetimage returned null!!" << std::endl;
            return -1;
        }

        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, (void*)(&xim->data[0]));
        XDestroyImage(xim);
    }

    return tex;
}
}

