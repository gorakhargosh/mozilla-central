/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Initial Developer of the Original Code is Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Matt Woodrow <mwoodrow@mozilla.com>
 *   Bas Schouten <bschouten@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#ifdef MOZ_WIDGET_GTK2
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#define GET_NATIVE_WINDOW(aWidget) GDK_WINDOW_XID((GdkWindow *) aWidget->GetNativeData(NS_NATIVE_WINDOW))
#elif defined(MOZ_WIDGET_QT)
#include <QWidget>
#include <QX11Info>
#define GET_NATIVE_WINDOW(aWidget) static_cast<QWidget*>(aWidget->GetNativeData(NS_NATIVE_SHELLWIDGET))->handle()
#endif

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "mozilla/X11Util.h"

#include "prenv.h"
#include "GLContextProvider.h"
#include "nsDebug.h"
#include "nsIWidget.h"
#include "GLXLibrary.h"
#include "gfxXlibSurface.h"
#include "gfxContext.h"
#include "gfxImageSurface.h"
#include "gfxPlatform.h"
#include "GLContext.h"
#include "gfxUtils.h"

#include "gfxCrashReporterUtils.h"

namespace mozilla {
namespace gl {

static PRBool gIsATI = PR_FALSE;
static PRBool gIsChromium = PR_FALSE;
static int gGLXMajorVersion = 0, gGLXMinorVersion = 0;

// Check that we have at least version aMajor.aMinor .
static inline bool
GLXVersionCheck(int aMajor, int aMinor)
{
    return aMajor < gGLXMajorVersion ||
           (aMajor == gGLXMajorVersion && aMinor <= gGLXMinorVersion);
}

static inline bool
HasExtension(const char* aExtensions, const char* aRequiredExtension)
{
    return GLContext::ListHasExtension(
        reinterpret_cast<const GLubyte*>(aExtensions), aRequiredExtension);
}

PRBool
GLXLibrary::EnsureInitialized()
{
    if (mInitialized) {
        return PR_TRUE;
    }

    // Don't repeatedly try to initialize.
    if (mTriedInitializing) {
        return PR_FALSE;
    }
    mTriedInitializing = PR_TRUE;

    if (!mOGLLibrary) {
        // see e.g. bug 608526: it is intrinsically interesting to know whether we have dynamically linked to libGL.so.1
        // because at least the NVIDIA implementation requires an executable stack, which causes mprotect calls,
        // which trigger glibc bug http://sourceware.org/bugzilla/show_bug.cgi?id=12225
        const char *libGLfilename = "libGL.so.1";
        ScopedGfxFeatureReporter reporter(libGLfilename);
        mOGLLibrary = PR_LoadLibrary(libGLfilename);
        if (!mOGLLibrary) {
            NS_WARNING("Couldn't load OpenGL shared library.");
            return PR_FALSE;
        }
        reporter.SetSuccessful();
    }

    LibrarySymbolLoader::SymLoadStruct symbols[] = {
        /* functions that were in GLX 1.0 */
        { (PRFuncPtr*) &xDestroyContext, { "glXDestroyContext", NULL } },
        { (PRFuncPtr*) &xMakeCurrent, { "glXMakeCurrent", NULL } },
        { (PRFuncPtr*) &xSwapBuffers, { "glXSwapBuffers", NULL } },
        { (PRFuncPtr*) &xQueryVersion, { "glXQueryVersion", NULL } },
        { (PRFuncPtr*) &xGetCurrentContext, { "glXGetCurrentContext", NULL } },
        { (PRFuncPtr*) &xWaitGL, { "glXWaitGL", NULL } },
        /* functions introduced in GLX 1.1 */
        { (PRFuncPtr*) &xQueryExtensionsString, { "glXQueryExtensionsString", NULL } },
        { (PRFuncPtr*) &xGetClientString, { "glXGetClientString", NULL } },
        { (PRFuncPtr*) &xQueryServerString, { "glXQueryServerString", NULL } },
        { NULL, { NULL } }
    };

    LibrarySymbolLoader::SymLoadStruct symbols13[] = {
        /* functions introduced in GLX 1.3 */
        { (PRFuncPtr*) &xChooseFBConfig, { "glXChooseFBConfig", NULL } },
        { (PRFuncPtr*) &xGetFBConfigAttrib, { "glXGetFBConfigAttrib", NULL } },
        // WARNING: xGetFBConfigs not set in symbols13_ext
        { (PRFuncPtr*) &xGetFBConfigs, { "glXGetFBConfigs", NULL } },
        { (PRFuncPtr*) &xGetVisualFromFBConfig, { "glXGetVisualFromFBConfig", NULL } },
        // WARNING: symbols13_ext sets xCreateGLXPixmapWithConfig instead
        { (PRFuncPtr*) &xCreatePixmap, { "glXCreatePixmap", NULL } },
        { (PRFuncPtr*) &xDestroyPixmap, { "glXDestroyPixmap", NULL } },
        { (PRFuncPtr*) &xCreateNewContext, { "glXCreateNewContext", NULL } },
        { NULL, { NULL } }
    };

    LibrarySymbolLoader::SymLoadStruct symbols13_ext[] = {
        /* extension equivalents for functions introduced in GLX 1.3 */
        // GLX_SGIX_fbconfig extension
        { (PRFuncPtr*) &xChooseFBConfig, { "glXChooseFBConfigSGIX", NULL } },
        { (PRFuncPtr*) &xGetFBConfigAttrib, { "glXGetFBConfigAttribSGIX", NULL } },
        // WARNING: no xGetFBConfigs equivalent in extensions
        { (PRFuncPtr*) &xGetVisualFromFBConfig, { "glXGetVisualFromFBConfig", NULL } },
        // WARNING: different from symbols13:
        { (PRFuncPtr*) &xCreateGLXPixmapWithConfig, { "glXCreateGLXPixmapWithConfigSGIX", NULL } },
        { (PRFuncPtr*) &xDestroyPixmap, { "glXDestroyGLXPixmap", NULL } }, // not from ext
        { (PRFuncPtr*) &xCreateNewContext, { "glXCreateContextWithConfigSGIX", NULL } },
        { NULL, { NULL } }
    };

    LibrarySymbolLoader::SymLoadStruct symbols14[] = {
        /* functions introduced in GLX 1.4 */
        { (PRFuncPtr*) &xGetProcAddress, { "glXGetProcAddress", NULL } },
        { NULL, { NULL } }
    };

    LibrarySymbolLoader::SymLoadStruct symbols14_ext[] = {
        /* extension equivalents for functions introduced in GLX 1.4 */
        // GLX_ARB_get_proc_address extension
        { (PRFuncPtr*) &xGetProcAddress, { "glXGetProcAddressARB", NULL } },
        { NULL, { NULL } }
    };

    LibrarySymbolLoader::SymLoadStruct symbols_texturefrompixmap[] = {
        { (PRFuncPtr*) &xBindTexImage, { "glXBindTexImageEXT", NULL } },
        { (PRFuncPtr*) &xReleaseTexImage, { "glXReleaseTexImageEXT", NULL } },
        { NULL, { NULL } }
    };

    if (!LibrarySymbolLoader::LoadSymbols(mOGLLibrary, &symbols[0])) {
        NS_WARNING("Couldn't find required entry point in OpenGL shared library");
        return PR_FALSE;
    }

    Display *display = DefaultXDisplay();

    int screen = DefaultScreen(display);
    const char *serverVendor = NULL;
    const char *serverVersionStr = NULL;
    const char *extensionsStr = NULL;

    if (!xQueryVersion(display, &gGLXMajorVersion, &gGLXMinorVersion)) {
        gGLXMajorVersion = 0;
        gGLXMinorVersion = 0;
        return PR_FALSE;
    }

    serverVendor = xQueryServerString(display, screen, GLX_VENDOR);
    serverVersionStr = xQueryServerString(display, screen, GLX_VERSION);

    if (!GLXVersionCheck(1, 1))
        // Not possible to query for extensions.
        return PR_FALSE;

    extensionsStr = xQueryExtensionsString(display, screen);

    LibrarySymbolLoader::SymLoadStruct *sym13;
    if (!GLXVersionCheck(1, 3)) {
        // Even if we don't have 1.3, we might have equivalent extensions
        // (as on the Intel X server).
        if (!HasExtension(extensionsStr, "GLX_SGIX_fbconfig")) {
            return PR_FALSE;
        }
        sym13 = symbols13_ext;
    } else {
        sym13 = symbols13;
    }
    if (!LibrarySymbolLoader::LoadSymbols(mOGLLibrary, sym13)) {
        NS_WARNING("Couldn't find required entry point in OpenGL shared library");
        return PR_FALSE;
    }

    LibrarySymbolLoader::SymLoadStruct *sym14;
    if (!GLXVersionCheck(1, 4)) {
        // Even if we don't have 1.4, we might have equivalent extensions
        // (as on the Intel X server).
        if (!HasExtension(extensionsStr, "GLX_ARB_get_proc_address")) {
            return PR_FALSE;
        }
        sym14 = symbols14_ext;
    } else {
        sym14 = symbols14;
    }
    if (!LibrarySymbolLoader::LoadSymbols(mOGLLibrary, sym14)) {
        NS_WARNING("Couldn't find required entry point in OpenGL shared library");
        return PR_FALSE;
    }

    if (HasExtension(extensionsStr, "GLX_EXT_texture_from_pixmap") &&
        LibrarySymbolLoader::LoadSymbols(mOGLLibrary, symbols_texturefrompixmap))
    {
        mHasTextureFromPixmap = PR_TRUE;
    }

    gIsATI = serverVendor && DoesVendorStringMatch(serverVendor, "ATI");
    gIsChromium = (serverVendor &&
                   DoesVendorStringMatch(serverVendor, "Chromium")) ||
        (serverVersionStr &&
         DoesVendorStringMatch(serverVersionStr, "Chromium"));

    mInitialized = PR_TRUE;
    return PR_TRUE;
}

GLXPixmap 
GLXLibrary::CreatePixmap(gfxASurface* aSurface)
{
    if (aSurface->GetType() != gfxASurface::SurfaceTypeXlib || !mHasTextureFromPixmap) {
        return 0;
    }

    if (!EnsureInitialized()) {
        return 0;
    }

    int attribs[] = { GLX_DOUBLEBUFFER, False,
                      GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
                      GLX_BIND_TO_TEXTURE_RGBA_EXT, True,
                      None };

    int numFormats;
    Display *display = DefaultXDisplay();
    int xscreen = DefaultScreen(display);

    ScopedXFree<GLXFBConfig> cfg(xChooseFBConfig(display,
                                                 xscreen,
                                                 attribs,
                                                 &numFormats));
    if (!cfg) {
        return 0;
    }
    NS_ABORT_IF_FALSE(numFormats > 0,
                 "glXChooseFBConfig() failed to match our requested format and violated its spec (!)");

    gfxXlibSurface *xs = static_cast<gfxXlibSurface*>(aSurface);

    int pixmapAttribs[] = { GLX_TEXTURE_TARGET_EXT, GLX_TEXTURE_2D_EXT,
                            GLX_TEXTURE_FORMAT_EXT, GLX_TEXTURE_FORMAT_RGBA_EXT,
                            None};

    GLXPixmap glxpixmap = xCreatePixmap(display,
                                        cfg[0],
                                        xs->XDrawable(),
                                        pixmapAttribs);

    return glxpixmap;
}

void
GLXLibrary::DestroyPixmap(GLXPixmap aPixmap)
{
    if (!mHasTextureFromPixmap) {
        return;
    }

    Display *display = DefaultXDisplay();
    xDestroyPixmap(display, aPixmap);
}

void
GLXLibrary::BindTexImage(GLXPixmap aPixmap)
{    
    if (!mHasTextureFromPixmap) {
        return;
    }

    Display *display = DefaultXDisplay();
    // Make sure all X drawing to the surface has finished before binding to a texture.
    XSync(DefaultXDisplay(), False);
    xBindTexImage(display, aPixmap, GLX_FRONT_LEFT_EXT, NULL);
}

void
GLXLibrary::ReleaseTexImage(GLXPixmap aPixmap)
{
    if (!mHasTextureFromPixmap) {
        return;
    }

    Display *display = DefaultXDisplay();
    xReleaseTexImage(display, aPixmap, GLX_FRONT_LEFT_EXT);
}

GLXLibrary sGLXLibrary;

class GLContextGLX : public GLContext
{
public:
    static already_AddRefed<GLContextGLX>
    CreateGLContext(const ContextFormat& format,
                    Display *display,
                    GLXDrawable drawable,
                    GLXFBConfig cfg,
                    XVisualInfo *vinfo,
                    GLContextGLX *shareContext,
                    PRBool deleteDrawable,
                    gfxXlibSurface *pixmap = nsnull)
    {
        int db = 0, err;
        err = sGLXLibrary.xGetFBConfigAttrib(display, cfg,
                                             GLX_DOUBLEBUFFER, &db);
        if (GLX_BAD_ATTRIBUTE != err) {
#ifdef DEBUG
            printf("[GLX] FBConfig is %sdouble-buffered\n", db ? "" : "not ");
#endif
        }

        GLXContext context;
        nsRefPtr<GLContextGLX> glContext;
        bool error;

        ScopedXErrorHandler xErrorHandler;

TRY_AGAIN_NO_SHARING:

        error = false;

        context = sGLXLibrary.xCreateNewContext(display,
                                                cfg,
                                                GLX_RGBA_TYPE,
                                                shareContext ? shareContext->mContext : NULL,
                                                True);

        if (context) {
            glContext = new GLContextGLX(format,
                                        shareContext,
                                        display,
                                        drawable,
                                        context,
                                        deleteDrawable,
                                        db,
                                        pixmap);
            if (!glContext->Init())
                error = true;
        } else {
            error = true;
        }

        error |= xErrorHandler.SyncAndGetError(display);

        if (error) {
            if (shareContext) {
                shareContext = nsnull;
                goto TRY_AGAIN_NO_SHARING;
            }

            NS_WARNING("Failed to create GLXContext!");
            glContext = nsnull; // note: this must be done while the graceful X error handler is set,
                                // because glxMakeCurrent can give a GLXBadDrawable error
        }

        return glContext.forget();
    }

    ~GLContextGLX()
    {
        MarkDestroyed();

        // see bug 659842 comment 76
#ifdef DEBUG
        bool success =
#endif
        sGLXLibrary.xMakeCurrent(mDisplay, None, nsnull);
        NS_ABORT_IF_FALSE(success,
            "glXMakeCurrent failed to release GL context before we call glXDestroyContext!");

        sGLXLibrary.xDestroyContext(mDisplay, mContext);

        if (mDeleteDrawable) {
            sGLXLibrary.xDestroyPixmap(mDisplay, mDrawable);
        }
    }

    GLContextType GetContextType() {
        return ContextTypeGLX;
    }

    PRBool Init()
    {
        MakeCurrent();
        SetupLookupFunction();
        if (!InitWithPrefix("gl", PR_TRUE)) {
            return PR_FALSE;
        }

        return IsExtensionSupported("GL_EXT_framebuffer_object");
    }

    PRBool MakeCurrentImpl(PRBool aForce = PR_FALSE)
    {
        PRBool succeeded = PR_TRUE;

        // With the ATI FGLRX driver, glxMakeCurrent is very slow even when the context doesn't change.
        // (This is not the case with other drivers such as NVIDIA).
        // So avoid calling it more than necessary. Since GLX documentation says that:
        //     "glXGetCurrentContext returns client-side information.
        //      It does not make a round trip to the server."
        // I assume that it's not worth using our own TLS slot here.
        if (aForce || sGLXLibrary.xGetCurrentContext() != mContext) {
            succeeded = sGLXLibrary.xMakeCurrent(mDisplay, mDrawable, mContext);
            NS_ASSERTION(succeeded, "Failed to make GL context current!");
        }

        return succeeded;
    }

    PRBool SetupLookupFunction()
    {
        mLookupFunc = (PlatformLookupFunction)sGLXLibrary.xGetProcAddress;
        return PR_TRUE;
    }

    void *GetNativeData(NativeDataType aType)
    {
        switch(aType) {
        case NativeGLContext:
            return mContext;
 
        case NativeThebesSurface:
            return mPixmap;

        default:
            return nsnull;
        }
    }

    PRBool IsDoubleBuffered()
    {
        return mDoubleBuffered;
    }

    PRBool SwapBuffers()
    {
        if (!mDoubleBuffered)
            return PR_FALSE;
        sGLXLibrary.xSwapBuffers(mDisplay, mDrawable);
        sGLXLibrary.xWaitGL();
        return PR_TRUE;
    }

    PRBool TextureImageSupportsGetBackingSurface()
    {
        return sGLXLibrary.HasTextureFromPixmap();
    }

    virtual already_AddRefed<TextureImage>
    CreateTextureImage(const nsIntSize& aSize,
                       TextureImage::ContentType aContentType,
                       GLenum aWrapMode,
                       PRBool aUseNearestFilter = PR_FALSE);

private:
    friend class GLContextProviderGLX;

    GLContextGLX(const ContextFormat& aFormat,
                 GLContext *aShareContext,
                 Display *aDisplay,
                 GLXDrawable aDrawable,
                 GLXContext aContext,
                 PRBool aDeleteDrawable,
                 PRBool aDoubleBuffered,
                 gfxXlibSurface *aPixmap)
        : GLContext(aFormat, aDeleteDrawable ? PR_TRUE : PR_FALSE, aShareContext),
          mContext(aContext),
          mDisplay(aDisplay),
          mDrawable(aDrawable),
          mDeleteDrawable(aDeleteDrawable),
          mDoubleBuffered(aDoubleBuffered),
          mPixmap(aPixmap)
    { }

    GLXContext mContext;
    Display *mDisplay;
    GLXDrawable mDrawable;
    PRPackedBool mDeleteDrawable;
    PRPackedBool mDoubleBuffered;

    nsRefPtr<gfxXlibSurface> mPixmap;
};

class TextureImageGLX : public TextureImage
{
    friend already_AddRefed<TextureImage>
    GLContextGLX::CreateTextureImage(const nsIntSize&,
                                     ContentType,
                                     GLenum,
                                     PRBool);

public:
    virtual ~TextureImageGLX()
    {
        mGLContext->MakeCurrent();
        mGLContext->fDeleteTextures(1, &mTexture);
        sGLXLibrary.DestroyPixmap(mPixmap);
    }

    virtual gfxASurface* BeginUpdate(nsIntRegion& aRegion)
    {
        mInUpdate = PR_TRUE;
        return mUpdateSurface;
    }

    virtual void EndUpdate()
    {
        mInUpdate = PR_FALSE;
    }

    virtual bool DirectUpdate(gfxASurface* aSurface, const nsIntRegion& aRegion)
    {
        nsRefPtr<gfxContext> ctx = new gfxContext(mUpdateSurface);
        gfxUtils::ClipToRegion(ctx, aRegion);
        ctx->SetSource(aSurface);
        ctx->SetOperator(gfxContext::OPERATOR_SOURCE);
        ctx->Paint();
        return true;
    }

    virtual void BindTexture(GLenum aTextureUnit)
    {
        mGLContext->fActiveTexture(aTextureUnit);
        mGLContext->fBindTexture(LOCAL_GL_TEXTURE_2D, Texture());
        sGLXLibrary.BindTexImage(mPixmap);
        mGLContext->fActiveTexture(LOCAL_GL_TEXTURE0);
    }

    virtual void ReleaseTexture()
    {
        sGLXLibrary.ReleaseTexImage(mPixmap);
    }

    virtual already_AddRefed<gfxASurface> GetBackingSurface()
    {
        NS_ADDREF(mUpdateSurface);
        return mUpdateSurface.get();
    }

    virtual PRBool InUpdate() const { return mInUpdate; }

private:
   TextureImageGLX(GLuint aTexture,
                   const nsIntSize& aSize,
                   GLenum aWrapMode,
                   ContentType aContentType,
                   GLContext* aContext,
                   gfxASurface* aSurface,
                   GLXPixmap aPixmap)
        : TextureImage(aTexture, aSize, aWrapMode, aContentType)
        , mGLContext(aContext)
        , mUpdateSurface(aSurface)
        , mPixmap(aPixmap)
        , mInUpdate(PR_FALSE)
    {
        if (aSurface->GetContentType() == gfxASurface::CONTENT_COLOR_ALPHA) {
            mShaderType = gl::RGBALayerProgramType;
        } else {
            mShaderType = gl::RGBXLayerProgramType;
        }
    }

    GLContext* mGLContext;
    nsRefPtr<gfxASurface> mUpdateSurface;
    GLXPixmap mPixmap;
    PRPackedBool mInUpdate;
};

already_AddRefed<TextureImage>
GLContextGLX::CreateTextureImage(const nsIntSize& aSize,
                                 TextureImage::ContentType aContentType,
                                 GLenum aWrapMode,
                                 PRBool aUseNearestFilter)
{
    if (!TextureImageSupportsGetBackingSurface()) {
        return GLContext::CreateTextureImage(aSize, 
                                             aContentType, 
                                             aWrapMode, 
                                             aUseNearestFilter);
    }

    Display *display = DefaultXDisplay();
    int xscreen = DefaultScreen(display);
    gfxASurface::gfxImageFormat imageFormat = gfxASurface::FormatFromContent(aContentType);

    XRenderPictFormat* xrenderFormat =
        gfxXlibSurface::FindRenderFormat(display, imageFormat);
    NS_ASSERTION(xrenderFormat, "Could not find a render format for our display!");


    nsRefPtr<gfxXlibSurface> surface =
        gfxXlibSurface::Create(ScreenOfDisplay(display, xscreen),
                               xrenderFormat,
                               gfxIntSize(aSize.width, aSize.height));
    NS_ASSERTION(surface, "Failed to create xlib surface!");

    if (aContentType == gfxASurface::CONTENT_COLOR_ALPHA) {
        nsRefPtr<gfxContext> ctx = new gfxContext(surface);
        ctx->SetOperator(gfxContext::OPERATOR_CLEAR);
        ctx->Paint();
    }

    MakeCurrent();
    GLXPixmap pixmap = sGLXLibrary.CreatePixmap(surface);
    NS_ASSERTION(pixmap, "Failed to create pixmap!");

    GLuint texture;
    fGenTextures(1, &texture);

    fActiveTexture(LOCAL_GL_TEXTURE0);
    fBindTexture(LOCAL_GL_TEXTURE_2D, texture);

    nsRefPtr<TextureImageGLX> teximage =
        new TextureImageGLX(texture, aSize, aWrapMode, aContentType, this, surface, pixmap);

    GLint texfilter = aUseNearestFilter ? LOCAL_GL_NEAREST : LOCAL_GL_LINEAR;
    fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MIN_FILTER, texfilter);
    fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAG_FILTER, texfilter);
    fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_S, aWrapMode);
    fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_WRAP_T, aWrapMode);

    return teximage.forget();
}

static GLContextGLX *
GetGlobalContextGLX()
{
    return static_cast<GLContextGLX*>(GLContextProviderGLX::GetGlobalContext());
}

static PRBool
AreCompatibleVisuals(XVisualInfo *one, XVisualInfo *two)
{
    if (one->c_class != two->c_class) {
        return PR_FALSE;
    }

    if (one->depth != two->depth) {
        return PR_FALSE;
    }	

    if (one->red_mask != two->red_mask ||
        one->green_mask != two->green_mask ||
        one->blue_mask != two->blue_mask) {
        return PR_FALSE;
    }

    if (one->bits_per_rgb != two->bits_per_rgb) {
        return PR_FALSE;
    }

    return PR_TRUE;
}

already_AddRefed<GLContext>
GLContextProviderGLX::CreateForWindow(nsIWidget *aWidget)
{
    if (!sGLXLibrary.EnsureInitialized()) {
        return nsnull;
    }

    // Currently, we take whatever Visual the window already has, and
    // try to create an fbconfig for that visual.  This isn't
    // necessarily what we want in the long run; an fbconfig may not
    // be available for the existing visual, or if it is, the GL
    // performance might be suboptimal.  But using the existing visual
    // is a relatively safe intermediate step.

    Display *display = (Display*)aWidget->GetNativeData(NS_NATIVE_DISPLAY); 
    int xscreen = DefaultScreen(display);
    Window window = GET_NATIVE_WINDOW(aWidget);

    int numConfigs;
    ScopedXFree<GLXFBConfig> cfgs;
    if (gIsATI || !GLXVersionCheck(1, 3)) {
        const int attribs[] = {
            GLX_DOUBLEBUFFER, False,
            0
        };
        cfgs = sGLXLibrary.xChooseFBConfig(display,
                                           xscreen,
                                           attribs,
                                           &numConfigs);
    } else {
        cfgs = sGLXLibrary.xGetFBConfigs(display,
                                         xscreen,
                                         &numConfigs);
    }

    if (!cfgs) {
        NS_WARNING("[GLX] glXGetFBConfigs() failed");
        return nsnull;
    }
    NS_ASSERTION(numConfigs > 0, "No FBConfigs found!");

    // XXX the visual ID is almost certainly the GLX_FBCONFIG_ID, so
    // we could probably do this first and replace the glXGetFBConfigs
    // with glXChooseConfigs.  Docs are sparklingly clear as always.
    XWindowAttributes widgetAttrs;
    if (!XGetWindowAttributes(display, window, &widgetAttrs)) {
        NS_WARNING("[GLX] XGetWindowAttributes() failed");
        return nsnull;
    }
    const VisualID widgetVisualID = XVisualIDFromVisual(widgetAttrs.visual);
#ifdef DEBUG
    printf("[GLX] widget has VisualID 0x%lx\n", widgetVisualID);
#endif

    ScopedXFree<XVisualInfo> vi;
    if (gIsATI) {
        XVisualInfo vinfo_template;
        int nvisuals;
        vinfo_template.visual   = widgetAttrs.visual;
        vinfo_template.visualid = XVisualIDFromVisual(vinfo_template.visual);
        vinfo_template.depth    = widgetAttrs.depth;
        vinfo_template.screen   = xscreen;
        vi = XGetVisualInfo(display, VisualIDMask|VisualDepthMask|VisualScreenMask,
                            &vinfo_template, &nvisuals);
        NS_ASSERTION(vi && nvisuals == 1, "Could not locate unique matching XVisualInfo for Visual");
    }

    int matchIndex = -1;
    ScopedXFree<XVisualInfo> vinfo;

    for (int i = 0; i < numConfigs; i++) {
        vinfo = sGLXLibrary.xGetVisualFromFBConfig(display, cfgs[i]);
        if (!vinfo) {
            continue;
        }
        if (gIsATI) {
            if (AreCompatibleVisuals(vi, vinfo)) {
                matchIndex = i;
                break;
            }
        } else {
            if (widgetVisualID == vinfo->visualid) {
                matchIndex = i;
                break;
            }
        }
    }

    if (matchIndex == -1) {
        NS_WARNING("[GLX] Couldn't find a FBConfig matching widget visual");
        return nsnull;
    }

    GLContextGLX *shareContext = GetGlobalContextGLX();

    nsRefPtr<GLContextGLX> glContext = GLContextGLX::CreateGLContext(ContextFormat(ContextFormat::BasicRGB24),
                                                                     display,
                                                                     window,
                                                                     cfgs[matchIndex],
                                                                     vinfo,
                                                                     shareContext,
                                                                     PR_FALSE);
    return glContext.forget();
}

static already_AddRefed<GLContextGLX>
CreateOffscreenPixmapContext(const gfxIntSize& aSize,
                             const ContextFormat& aFormat,
                             PRBool aShare)
{
    if (!sGLXLibrary.EnsureInitialized()) {
        return nsnull;
    }

    Display *display = DefaultXDisplay();
    int xscreen = DefaultScreen(display);

    int attribs[] = {
        GLX_DOUBLEBUFFER, False,
        GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT,
        GLX_X_RENDERABLE, True,
        GLX_RED_SIZE, 1,
        GLX_GREEN_SIZE, 1,
        GLX_BLUE_SIZE, 1,
        GLX_ALPHA_SIZE, 0,
        GLX_DEPTH_SIZE, 0,
        0
    };
    int numConfigs = 0;

    ScopedXFree<GLXFBConfig> cfgs;
    cfgs = sGLXLibrary.xChooseFBConfig(display,
                                       xscreen,
                                       attribs,
                                       &numConfigs);
    if (!cfgs) {
        return nsnull;
    }

    NS_ASSERTION(numConfigs > 0,
                 "glXChooseFBConfig() failed to match our requested format and violated its spec (!)");

    ScopedXFree<XVisualInfo> vinfo;
    int chosenIndex = 0;

    for (int i = 0; i < numConfigs; ++i) {
        int dtype, visid;

        if (sGLXLibrary.xGetFBConfigAttrib(display, cfgs[i], GLX_DRAWABLE_TYPE, &dtype) != Success
            || !(dtype & GLX_PIXMAP_BIT))
        {
            continue;
        }
        if (sGLXLibrary.xGetFBConfigAttrib(display, cfgs[i], GLX_VISUAL_ID, &visid) != Success
            || visid == 0)
        {
            continue;
        }

        vinfo = sGLXLibrary.xGetVisualFromFBConfig(display, cfgs[i]);

        if (vinfo) {
            chosenIndex = i;
            break;
        }
    }

    if (!vinfo) {
        NS_WARNING("glXChooseFBConfig() didn't give us any configs with visuals!");
        return nsnull;
    }

    ScopedXErrorHandler xErrorHandler;
    GLXPixmap glxpixmap = 0;
    bool error = false;

    nsRefPtr<gfxXlibSurface> xsurface = gfxXlibSurface::Create(DefaultScreenOfDisplay(display),
                                                               vinfo->visual,
                                                               gfxIntSize(16, 16));
    if (xsurface->CairoStatus() != 0) {
        error = true;
        goto DONE_CREATING_PIXMAP;
    }

    // Handle slightly different signature between glXCreatePixmap and
    // its pre-GLX-1.3 extension equivalent (though given the ABI, we
    // might not need to).
    if (GLXVersionCheck(1, 3)) {
        glxpixmap = sGLXLibrary.xCreatePixmap(display,
                                              cfgs[chosenIndex],
                                              xsurface->XDrawable(),
                                              NULL);
    } else {
        glxpixmap = sGLXLibrary.xCreateGLXPixmapWithConfig(display,
                                                           cfgs[chosenIndex],
                                                           xsurface->
                                                             XDrawable());
    }
    if (glxpixmap == 0) {
        error = true;
    }

DONE_CREATING_PIXMAP:

    nsRefPtr<GLContextGLX> glContext;
    bool serverError = xErrorHandler.SyncAndGetError(display);

    if (!error && // earlier recorded error
        !serverError)
    {
        glContext = GLContextGLX::CreateGLContext(
                        aFormat,
                        display,
                        glxpixmap,
                        cfgs[chosenIndex],
                        vinfo,
                        aShare ? GetGlobalContextGLX() : nsnull,
                        PR_TRUE,
                        xsurface);
    }

    return glContext.forget();
}

already_AddRefed<GLContext>
GLContextProviderGLX::CreateOffscreen(const gfxIntSize& aSize,
                                      const ContextFormat& aFormat)
{

    nsRefPtr<GLContextGLX> glContext =
        CreateOffscreenPixmapContext(aSize, aFormat, PR_TRUE);

    if (!glContext) {
        return nsnull;
    }

    if (!glContext->GetSharedContext()) {
        // no point in returning anything if sharing failed, we can't
        // render from this
        return nsnull;
    }

    if (!glContext->ResizeOffscreenFBO(aSize)) {
        // we weren't able to create the initial
        // offscreen FBO, so this is dead
        return nsnull;
    }

    return glContext.forget();
}

already_AddRefed<GLContext>
GLContextProviderGLX::CreateForNativePixmapSurface(gfxASurface *aSurface)
{
    if (!sGLXLibrary.EnsureInitialized()) {
        return nsnull;
    }

    if (aSurface->GetType() != gfxASurface::SurfaceTypeXlib) {
        NS_WARNING("GLContextProviderGLX::CreateForNativePixmapSurface called with non-Xlib surface");
        return nsnull;
    }

    nsAutoTArray<int, 20> attribs;

#define A1_(_x)  do { attribs.AppendElement(_x); } while(0)
#define A2_(_x,_y)  do {                                                \
        attribs.AppendElement(_x);                                      \
        attribs.AppendElement(_y);                                      \
    } while(0)

    A2_(GLX_DOUBLEBUFFER, False);
    A2_(GLX_DRAWABLE_TYPE, GLX_PIXMAP_BIT);
    A1_(0);

    int numFormats;
    Display *display = DefaultXDisplay();
    int xscreen = DefaultScreen(display);

    ScopedXFree<GLXFBConfig> cfg(sGLXLibrary.xChooseFBConfig(display,
                                                             xscreen,
                                                             attribs.Elements(),
                                                             &numFormats));
    if (!cfg) {
        return nsnull;
    }
    NS_ASSERTION(numFormats > 0,
                 "glXChooseFBConfig() failed to match our requested format and violated its spec (!)");

    gfxXlibSurface *xs = static_cast<gfxXlibSurface*>(aSurface);

    GLXPixmap glxpixmap = sGLXLibrary.xCreatePixmap(display,
                                                    cfg[0],
                                                    xs->XDrawable(),
                                                    NULL);

    nsRefPtr<GLContextGLX> glContext = GLContextGLX::CreateGLContext(ContextFormat(ContextFormat::BasicRGB24),
                                                                     display,
                                                                     glxpixmap,
                                                                     cfg[0],
                                                                     NULL,
                                                                     NULL,
                                                                     PR_FALSE,
                                                                     xs);

    return glContext.forget();
}

static nsRefPtr<GLContext> gGlobalContext;

GLContext *
GLContextProviderGLX::GetGlobalContext()
{
    static bool triedToCreateContext = false;
    if (!triedToCreateContext && !gGlobalContext) {
        triedToCreateContext = true;
        gGlobalContext = CreateOffscreenPixmapContext(gfxIntSize(1, 1),
                                                      ContextFormat(ContextFormat::BasicRGB24),
                                                      PR_FALSE);
        if (gGlobalContext)
            gGlobalContext->SetIsGlobalSharedContext(PR_TRUE);
    }

    return gGlobalContext;
}

void
GLContextProviderGLX::Shutdown()
{
    gGlobalContext = nsnull;
}

} /* namespace gl */
} /* namespace mozilla */
