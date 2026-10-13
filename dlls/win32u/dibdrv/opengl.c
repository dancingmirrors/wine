/*
 * DIB driver OpenGL support
 *
 * Copyright 2012 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <sys/types.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "ntgdi_private.h"
#include "dibdrv.h"
#include "wine/wgl.h"
#include "wine/wgl_driver.h"
#include "wine/debug.h"

#ifdef SONAME_LIBEGL

WINE_DEFAULT_DEBUG_CHANNEL(dib);

typedef int32_t EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLDeviceEXT;
typedef void *EGLDisplay;
typedef void *EGLSurface;

#define EGL_NO_CONTEXT                  ((EGLContext)0)
#define EGL_NO_DISPLAY                  ((EGLDisplay)0)
#define EGL_NO_SURFACE                  ((EGLSurface)0)
#define EGL_DEFAULT_DISPLAY             ((void *)0)

#define EGL_PBUFFER_BIT                 0x0001
#define EGL_OPENGL_BIT                  0x0008
#define EGL_ALPHA_SIZE                  0x3021
#define EGL_BLUE_SIZE                   0x3022
#define EGL_GREEN_SIZE                  0x3023
#define EGL_RED_SIZE                    0x3024
#define EGL_DEPTH_SIZE                  0x3025
#define EGL_STENCIL_SIZE                0x3026
#define EGL_CONFIG_ID                   0x3028
#define EGL_SURFACE_TYPE                0x3033
#define EGL_NONE                        0x3038
#define EGL_COLOR_BUFFER_TYPE           0x303f
#define EGL_RENDERABLE_TYPE             0x3040
#define EGL_VENDOR                      0x3053
#define EGL_EXTENSIONS                  0x3055
#define EGL_HEIGHT                      0x3056
#define EGL_WIDTH                       0x3057
#define EGL_RGB_BUFFER                  0x308e
#define EGL_OPENGL_API                  0x30a2
#define EGL_PLATFORM_DEVICE_EXT         0x313f
#define EGL_PLATFORM_SURFACELESS_MESA   0x31dd

struct wgl_context
{
    EGLConfig  config;
    EGLContext context;
    EGLContext helper;
    EGLSurface pbuffer;
    int        width;
    int        height;
    HDC        hdc;
    HBITMAP    bitmap;
    BOOL       has_been_current;
    BOOL       sharing;
};

static void *egl_handle;
static EGLDisplay egl_display;
static pthread_key_t current_context_key;
static struct opengl_funcs opengl_funcs;

#define USE_GL_FUNC(name) #name,
static const char *opengl_func_names[] = { ALL_WGL_FUNCS };
#undef USE_GL_FUNC

static EGLBoolean (*p_eglBindAPI)( EGLenum api );
static EGLBoolean (*p_eglChooseConfig)( EGLDisplay dpy, const EGLint *attrib_list, EGLConfig *configs,
                                        EGLint config_size, EGLint *num_config );
static EGLContext (*p_eglCreateContext)( EGLDisplay dpy, EGLConfig config, EGLContext share_context,
                                         const EGLint *attrib_list );
static EGLSurface (*p_eglCreatePbufferSurface)( EGLDisplay dpy, EGLConfig config, const EGLint *attrib_list );
static EGLBoolean (*p_eglDestroyContext)( EGLDisplay dpy, EGLContext ctx );
static EGLBoolean (*p_eglDestroySurface)( EGLDisplay dpy, EGLSurface surface );
static EGLBoolean (*p_eglGetConfigAttrib)( EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value );
static EGLDisplay (*p_eglGetDisplay)( void *display_id );
static EGLint (*p_eglGetError)(void);
static EGLDisplay (*p_eglGetPlatformDisplay)( EGLenum platform, void *native_display, const void *attrib_list );
static void *(*p_eglGetProcAddress)( const char *procname );
static EGLBoolean (*p_eglInitialize)( EGLDisplay dpy, EGLint *major, EGLint *minor );
static EGLBoolean (*p_eglMakeCurrent)( EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx );
static EGLBoolean (*p_eglQueryDevicesEXT)( EGLint max_devices, EGLDeviceEXT *devices, EGLint *num_devices );
static const char *(*p_eglQueryString)( EGLDisplay dpy, EGLint name );

static void (*p_glDrawPixels)( GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels );
static void (*p_glFinish)(void);
static void (*p_glFlush)(void);
static void (*p_glPixelStorei)( GLenum pname, GLint param );
static void (*p_glRasterPos2i)( GLint x, GLint y );
static void (*p_glReadPixels)( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels );
static void (*p_glViewport)( GLint x, GLint y, GLsizei width, GLsizei height );

static BOOL has_extension( const char *list, const char *ext )
{
    size_t len = strlen( ext );
    const char *p;

    if (!list) return FALSE;
    while ((p = strstr( list, ext )))
    {
        if ((p == list || p[-1] == ' ') && (p[len] == ' ' || !p[len])) return TRUE;
        list = p + len;
    }
    return FALSE;
}

static EGLDisplay init_display( EGLDisplay display, const char *platform )
{
    EGLint major, minor;

    if (display == EGL_NO_DISPLAY)
    {
        TRACE( "no %s display\n", platform );
        return EGL_NO_DISPLAY;
    }
    if (!p_eglInitialize( display, &major, &minor ))
    {
        WARN( "failed to initialize the %s display, error %#x\n", platform, p_eglGetError() );
        return EGL_NO_DISPLAY;
    }
    TRACE( "using %s display %p, EGL %d.%d, vendor %s\n", platform, display, major, minor,
           debugstr_a( p_eglQueryString( display, EGL_VENDOR )));
    return display;
}

static struct wgl_context *get_current_context(void)
{
    return pthread_getspecific( current_context_key );
}

static HBITMAP get_dc_bitmap( HDC hdc, int *width, int *height )
{
    HBITMAP bitmap = NtGdiGetDCObject( hdc, NTGDI_OBJ_SURF );
    BITMAPOBJ *bmp;
    dib_info dib;

    if (!(bmp = GDI_GetObjPtr( bitmap, NTGDI_OBJ_BITMAP ))) return 0;
    if (!init_dib_info_from_bitmapobj( &dib, bmp )) bitmap = 0;
    else
    {
        *width = dib.rect.right - dib.rect.left;
        *height = dib.rect.bottom - dib.rect.top;
    }
    GDI_ReleaseObj( bitmap );
    return bitmap;
}

static void copy_pixels( struct wgl_context *context, dib_info *dib, BOOL upload )
{
    int dib_width = dib->rect.right - dib->rect.left, dib_height = dib->rect.bottom - dib->rect.top;
    int width = min( dib_width, context->width ), height = min( dib_height, context->height );
    RECT rect = { 0, 0, dib_width, dib_height };
    void *bits, *buffer = NULL;
    int row_length;
    dib_info tmp;

    if (width <= 0 || height <= 0) return;

    if (!context->helper)
    {
        p_eglBindAPI( EGL_OPENGL_API );
        if (!(context->helper = p_eglCreateContext( egl_display, context->config, EGL_NO_CONTEXT, NULL )))
        {
            WARN( "failed to create the helper context, error %#x\n", p_eglGetError() );
            return;
        }
    }

    if (dib->funcs == &funcs_8888)
    {
        if (dib->stride < 0) bits = (char *)dib->bits.ptr + (dib->rect.bottom - 1) * dib->stride;
        else bits = (char *)dib->bits.ptr + dib->rect.top * dib->stride;
        bits = (char *)bits + dib->rect.left * 4;
        row_length = abs( dib->stride ) / 4;
    }
    else
    {
        BITMAPINFO info = {{ sizeof(info.bmiHeader), dib_width, dib->stride < 0 ? dib_height : -dib_height, 1, 32, BI_RGB }};

        if (!(buffer = malloc( dib_width * dib_height * 4 ))) return;
        init_dib_info_from_bitmapinfo( &tmp, &info, buffer );
        if (upload || width < dib_width || height < dib_height) tmp.funcs->convert_to( &tmp, dib, &rect, FALSE );
        bits = buffer;
        row_length = dib_width;
    }

    if (!upload) p_glFinish();

    if (!p_eglMakeCurrent( egl_display, context->pbuffer, context->pbuffer, context->helper ))
    {
        WARN( "failed to make the helper context current, error %#x\n", p_eglGetError() );
        free( buffer );
        return;
    }
    if (upload)
    {
        p_glPixelStorei( GL_UNPACK_ROW_LENGTH, row_length );
        p_glViewport( 0, 0, context->width, context->height );
        p_glRasterPos2i( -1, -1 );
        p_glDrawPixels( width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, bits );
        p_glFinish();
    }
    else
    {
        p_glPixelStorei( GL_PACK_ROW_LENGTH, row_length );
        p_glReadPixels( 0, 0, width, height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, bits );
    }
    p_eglMakeCurrent( egl_display, context->pbuffer, context->pbuffer, context->context );

    if (buffer)
    {
        if (!upload) dib->funcs->convert_to( dib, &tmp, &rect, FALSE );
        free( buffer );
    }
}

static void sync_bitmap( struct wgl_context *context, BOOL upload )
{
    HBITMAP bitmap = NtGdiGetDCObject( context->hdc, NTGDI_OBJ_SURF );
    BITMAPOBJ *bmp;
    dib_info dib;

    if (bitmap != context->bitmap)
    {
        TRACE( "context %p bitmap changed from %p to %p\n", context, context->bitmap, bitmap );
        context->bitmap = bitmap;
        upload = TRUE;
    }
    if (!(bmp = GDI_GetObjPtr( bitmap, NTGDI_OBJ_BITMAP ))) return;
    if (init_dib_info_from_bitmapobj( &dib, bmp ))
    {
        TRACE( "%s context %p pbuffer %dx%d bitmap %p %dx%d %d bpp\n", upload ? "uploading to" : "downloading from",
               context, context->width, context->height, bitmap, dib.rect.right - dib.rect.left,
               dib.rect.bottom - dib.rect.top, dib.bit_count );
        copy_pixels( context, &dib, upload );
    }
    GDI_ReleaseObj( bitmap );
}

static void sync_current_context(void)
{
    struct wgl_context *context = get_current_context();
    if (context) sync_bitmap( context, FALSE );
}

static void dib_glDrawPixels( GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels )
{
    sync_current_context();
    p_glDrawPixels( width, height, format, type, pixels );
}

static void dib_glFinish(void)
{
    p_glFinish();
    sync_current_context();
}

static void dib_glFlush(void)
{
    p_glFlush();
    sync_current_context();
}

static void dib_glReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels )
{
    sync_current_context();
    p_glReadPixels( x, y, width, height, format, type, pixels );
}

static void dib_glViewport( GLint x, GLint y, GLsizei width, GLsizei height )
{
    sync_current_context();
    p_glViewport( x, y, width, height );
}

static void init_opengl(void)
{
    EGLDisplay (*get_platform_display)( EGLenum platform, void *native_display, const void *attrib_list );
    const char *extensions;
    unsigned int i;

    if (!(egl_handle = dlopen( SONAME_LIBEGL, RTLD_NOW )))
    {
        ERR( "Failed to load %s: %s\n", SONAME_LIBEGL, dlerror() );
        return;
    }

#define LOAD_FUNCPTR(f) do if (!(p_##f = dlsym( egl_handle, #f ))) \
    { \
        ERR( "%s not found in %s, disabling.\n", #f, SONAME_LIBEGL ); \
        goto failed; \
    } while(0)

    LOAD_FUNCPTR( eglBindAPI );
    LOAD_FUNCPTR( eglChooseConfig );
    LOAD_FUNCPTR( eglCreateContext );
    LOAD_FUNCPTR( eglCreatePbufferSurface );
    LOAD_FUNCPTR( eglDestroyContext );
    LOAD_FUNCPTR( eglDestroySurface );
    LOAD_FUNCPTR( eglGetConfigAttrib );
    LOAD_FUNCPTR( eglGetDisplay );
    LOAD_FUNCPTR( eglGetError );
    LOAD_FUNCPTR( eglGetProcAddress );
    LOAD_FUNCPTR( eglInitialize );
    LOAD_FUNCPTR( eglMakeCurrent );
    LOAD_FUNCPTR( eglQueryString );
#undef LOAD_FUNCPTR

    extensions = p_eglQueryString( EGL_NO_DISPLAY, EGL_EXTENSIONS );
    TRACE( "EGL client extensions: %s\n", debugstr_a( extensions ));

    if (!(get_platform_display = p_eglGetPlatformDisplay = dlsym( egl_handle, "eglGetPlatformDisplay" )) &&
        has_extension( extensions, "EGL_EXT_platform_base" ))
        get_platform_display = p_eglGetProcAddress( "eglGetPlatformDisplayEXT" );

    if (get_platform_display && has_extension( extensions, "EGL_MESA_platform_surfaceless" ))
        egl_display = init_display( get_platform_display( EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL ),
                                    "surfaceless" );

    if (egl_display == EGL_NO_DISPLAY && get_platform_display && has_extension( extensions, "EGL_EXT_platform_device" ) &&
        (p_eglQueryDevicesEXT = p_eglGetProcAddress( "eglQueryDevicesEXT" )))
    {
        EGLDeviceEXT devices[16];
        EGLint count = 0;

        if (p_eglQueryDevicesEXT( ARRAY_SIZE(devices), devices, &count ))
            for (i = 0; i < count && egl_display == EGL_NO_DISPLAY; i++)
                egl_display = init_display( get_platform_display( EGL_PLATFORM_DEVICE_EXT, devices[i], NULL ), "device" );
    }

    if (egl_display == EGL_NO_DISPLAY)
        egl_display = init_display( p_eglGetDisplay( EGL_DEFAULT_DISPLAY ), "default" );

    if (egl_display == EGL_NO_DISPLAY)
    {
        ERR( "Failed to initialize an EGL display, disabling.\n" );
        goto failed;
    }
    if (!p_eglBindAPI( EGL_OPENGL_API ))
    {
        ERR( "OpenGL is not supported by %s, disabling.\n", SONAME_LIBEGL );
        goto failed;
    }

    for (i = 0; i < ARRAY_SIZE( opengl_func_names ); i++)
    {
        if (!(((void **)&opengl_funcs.gl)[i] = p_eglGetProcAddress( opengl_func_names[i] )))
        {
            ERR( "%s not found in %s, disabling.\n", opengl_func_names[i], SONAME_LIBEGL );
            goto failed;
        }
    }

    p_glPixelStorei = opengl_funcs.gl.p_glPixelStorei;
    p_glRasterPos2i = opengl_funcs.gl.p_glRasterPos2i;

#define WRAP_GL_FUNC(name) do { p_##name = opengl_funcs.gl.p_##name; opengl_funcs.gl.p_##name = dib_##name; } while(0)
    WRAP_GL_FUNC( glDrawPixels );
    WRAP_GL_FUNC( glFinish );
    WRAP_GL_FUNC( glFlush );
    WRAP_GL_FUNC( glReadPixels );
    WRAP_GL_FUNC( glViewport );
#undef WRAP_GL_FUNC

    if (pthread_key_create( &current_context_key, NULL ))
    {
        ERR( "Failed to create the thread key, disabling.\n" );
        goto failed;
    }
    return;

failed:
    dlclose( egl_handle );
    egl_handle = NULL;
}

/***********************************************************************
 *		egl_get_gl_funcs
 */
static void egl_get_gl_funcs( struct opengl_funcs *funcs )
{
    funcs->gl = opengl_funcs.gl;
}

static EGLConfig choose_config( const PIXELFORMATDESCRIPTOR *descr )
{
    static const EGLint depth_sizes[] = { 32, 24, 16, 0 };
    EGLint alpha_size = descr->cAlphaBits ? 8 : 0;
    EGLConfig configs[64];
    EGLint attribs[32], *attr, count, i, j;

    for (i = 0; i < ARRAY_SIZE(depth_sizes); i++)
    {
        if (depth_sizes[i] > descr->cDepthBits) continue;

        attr = attribs;
        *attr++ = EGL_SURFACE_TYPE;     *attr++ = EGL_PBUFFER_BIT;
        *attr++ = EGL_RENDERABLE_TYPE;  *attr++ = EGL_OPENGL_BIT;
        *attr++ = EGL_COLOR_BUFFER_TYPE; *attr++ = EGL_RGB_BUFFER;
        *attr++ = EGL_RED_SIZE;         *attr++ = 8;
        *attr++ = EGL_GREEN_SIZE;       *attr++ = 8;
        *attr++ = EGL_BLUE_SIZE;        *attr++ = 8;
        *attr++ = EGL_ALPHA_SIZE;       *attr++ = alpha_size;
        *attr++ = EGL_DEPTH_SIZE;       *attr++ = depth_sizes[i];
        *attr++ = EGL_STENCIL_SIZE;     *attr++ = descr->cStencilBits;
        *attr++ = EGL_NONE;

        if (!p_eglChooseConfig( egl_display, attribs, configs, ARRAY_SIZE(configs), &count ) || !count) continue;

        for (j = 0; j < count; j++)
        {
            EGLint red, green, blue, alpha, id;

            p_eglGetConfigAttrib( egl_display, configs[j], EGL_RED_SIZE, &red );
            p_eglGetConfigAttrib( egl_display, configs[j], EGL_GREEN_SIZE, &green );
            p_eglGetConfigAttrib( egl_display, configs[j], EGL_BLUE_SIZE, &blue );
            p_eglGetConfigAttrib( egl_display, configs[j], EGL_ALPHA_SIZE, &alpha );
            if (red != 8 || green != 8 || blue != 8 || alpha != alpha_size) continue;
            p_eglGetConfigAttrib( egl_display, configs[j], EGL_CONFIG_ID, &id );
            TRACE( "using config %d for %u bpp alpha %u depth %u stencil %u\n", id, descr->cColorBits,
                   descr->cAlphaBits, descr->cDepthBits, descr->cStencilBits );
            return configs[j];
        }
        return configs[0];
    }
    return NULL;
}

/***********************************************************************
 *		egl_create_context
 */
static struct wgl_context *egl_create_context( HDC hdc, const PIXELFORMATDESCRIPTOR *descr )
{
    struct wgl_context *context;

    if (!(context = calloc( 1, sizeof(*context) ))) return NULL;
    if (!(context->config = choose_config( descr )))
    {
        WARN( "no EGL config for %u bpp alpha %u depth %u stencil %u\n", descr->cColorBits,
              descr->cAlphaBits, descr->cDepthBits, descr->cStencilBits );
        free( context );
        return NULL;
    }
    p_eglBindAPI( EGL_OPENGL_API );
    if (!(context->context = p_eglCreateContext( egl_display, context->config, EGL_NO_CONTEXT, NULL )))
    {
        ERR( "failed to create an EGL context, error %#x\n", p_eglGetError() );
        free( context );
        return NULL;
    }
    TRACE( "created context %p (%p)\n", context, context->context );
    return context;
}

/***********************************************************************
 *		egl_delete_context
 */
static BOOL egl_delete_context( struct wgl_context *context )
{
    TRACE( "deleting context %p (%p)\n", context, context->context );

    if (get_current_context() == context)
    {
        p_eglMakeCurrent( egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
        pthread_setspecific( current_context_key, NULL );
    }
    if (context->pbuffer) p_eglDestroySurface( egl_display, context->pbuffer );
    if (context->helper) p_eglDestroyContext( egl_display, context->helper );
    p_eglDestroyContext( egl_display, context->context );
    free( context );
    return TRUE;
}

/***********************************************************************
 *		egl_get_proc_address
 */
static PROC egl_get_proc_address( const char *proc )
{
    return (PROC)p_eglGetProcAddress( proc );
}

/***********************************************************************
 *		egl_make_current
 */
static BOOL egl_make_current( HDC hdc, struct wgl_context *context )
{
    struct wgl_context *prev = get_current_context();
    EGLSurface old_pbuffer = EGL_NO_SURFACE;
    HBITMAP bitmap;
    int width, height;

    TRACE( "hdc %p context %p\n", hdc, context );

    if (prev)
    {
        p_eglMakeCurrent( egl_display, prev->pbuffer, prev->pbuffer, prev->context );
        sync_bitmap( prev, FALSE );
    }

    if (!context)
    {
        p_eglMakeCurrent( egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
        pthread_setspecific( current_context_key, NULL );
        return TRUE;
    }

    if (!(bitmap = get_dc_bitmap( hdc, &width, &height )))
    {
        WARN( "no bitmap selected in %p\n", hdc );
        return FALSE;
    }

    if (context->pbuffer == EGL_NO_SURFACE || context->width != width || context->height != height)
    {
        EGLint attribs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
        EGLSurface pbuffer;

        if (!(pbuffer = p_eglCreatePbufferSurface( egl_display, context->config, attribs )))
        {
            ERR( "failed to create a %dx%d pbuffer, error %#x\n", width, height, p_eglGetError() );
            return FALSE;
        }
        old_pbuffer = context->pbuffer;
        context->pbuffer = pbuffer;
    }

    p_eglBindAPI( EGL_OPENGL_API );
    if (!p_eglMakeCurrent( egl_display, context->pbuffer, context->pbuffer, context->context ))
    {
        ERR( "failed to make context %p current, error %#x\n", context, p_eglGetError() );
        if (old_pbuffer != EGL_NO_SURFACE)
        {
            p_eglDestroySurface( egl_display, context->pbuffer );
            context->pbuffer = old_pbuffer;
        }
        return FALSE;
    }
    if (old_pbuffer != EGL_NO_SURFACE) p_eglDestroySurface( egl_display, old_pbuffer );

    context->width = width;
    context->height = height;
    context->hdc = hdc;
    context->has_been_current = TRUE;
    pthread_setspecific( current_context_key, context );

    sync_bitmap( context, TRUE );
    return TRUE;
}

/***********************************************************************
 *		egl_share_lists
 */
static BOOL egl_share_lists( struct wgl_context *org, struct wgl_context *dest )
{
    struct wgl_context *keep, *clobber;
    EGLContext context;

    TRACE( "org %p dest %p\n", org, dest );

    if (!dest->has_been_current && !dest->sharing)
    {
        keep = org;
        clobber = dest;
    }
    else if (!org->has_been_current && !org->sharing)
    {
        keep = dest;
        clobber = org;
    }
    else
    {
        ERR( "Could not share display lists because both of the contexts have already been current or shared\n" );
        return FALSE;
    }

    p_eglBindAPI( EGL_OPENGL_API );
    if (!(context = p_eglCreateContext( egl_display, clobber->config, keep->context, NULL )))
    {
        ERR( "failed to re-create context %p, error %#x\n", clobber, p_eglGetError() );
        return FALSE;
    }
    p_eglDestroyContext( egl_display, clobber->context );
    clobber->context = context;
    TRACE( "re-created context %p (%p) sharing lists with %p (%p)\n", clobber, clobber->context, keep, keep->context );

    org->sharing = TRUE;
    dest->sharing = TRUE;
    return TRUE;
}

/***********************************************************************
 *		egl_swap_buffers
 */
static BOOL egl_swap_buffers( HDC hdc )
{
    struct wgl_context *context = get_current_context();

    if (!context || context->hdc != hdc) return TRUE;
    p_glFlush();
    sync_bitmap( context, FALSE );
    return TRUE;
}

static const struct dibdrv_gl_funcs egl_funcs =
{
    egl_get_gl_funcs,
    egl_create_context,
    egl_delete_context,
    egl_get_proc_address,
    egl_make_current,
    egl_share_lists,
    egl_swap_buffers,
};

const struct dibdrv_gl_funcs *init_opengl_lib(void)
{
    static pthread_once_t init_once = PTHREAD_ONCE_INIT;

    pthread_once( &init_once, init_opengl );
    if (!egl_handle) return NULL;
    return &egl_funcs;
}

#else  /* SONAME_LIBEGL */

const struct dibdrv_gl_funcs *init_opengl_lib(void)
{
    return NULL;
}

#endif  /* SONAME_LIBEGL */
