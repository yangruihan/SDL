/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2015 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_WINRT

/* WinRT SDL video driver implementation

   Initial work on this was done by David Ludwig (dludwig@pobox.com), and
   was based off of SDL's "dummy" video driver.
 */

/* Windows includes */
#include <agile.h>
#include <windows.graphics.display.h>
#include <dxgi.h>
#include <dxgi1_2.h>
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;
using namespace Windows::UI::Core;
using namespace Windows::UI::ViewManagement;


/* [re]declare Windows GUIDs locally, to limit the amount of external lib(s) SDL has to link to */
static const GUID IID_IDXGIFactory2 = { 0x50c83a1c, 0xe072, 0x4c48,{ 0x87, 0xb0, 0x36, 0x30, 0xfa, 0x36, 0xa6, 0xd0 } };


/* SDL includes */
extern "C" {
#include "SDL_video.h"
#include "SDL_mouse.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../render/SDL_sysrender.h"
#include "SDL_syswm.h"
#include "SDL_winrtopengles.h"
#include "../../core/windows/SDL_windows.h"
}

#include "../../core/winrt/SDL_winrtapp_direct3d.h"
#include "../../core/winrt/SDL_winrtapp_xaml.h"
#include "SDL_winrtvideo_cpp.h"
#include "SDL_winrtevents_c.h"
#include "SDL_winrtmouse_c.h"
#include "SDL_main.h"
#include "SDL_system.h"
//#include "SDL_log.h"


/* Initialization/Query functions */
static int WINRT_VideoInit(_THIS);
static int WINRT_InitModes(_THIS);
static int WINRT_SetDisplayMode(_THIS, SDL_VideoDisplay * display, SDL_DisplayMode * mode);
static void WINRT_VideoQuit(_THIS);


/* Window functions */
static int WINRT_CreateWindow(_THIS, SDL_Window * window);
static void WINRT_DestroyWindow(_THIS, SDL_Window * window);
static SDL_bool WINRT_GetWindowWMInfo(_THIS, SDL_Window * window, SDL_SysWMinfo * info);


/* SDL-internal globals: */
SDL_Window * WINRT_GlobalSDLWindow = NULL;


/* WinRT driver bootstrap functions */

static int
WINRT_Available(void)
{
    return (1);
}

static void
WINRT_DeleteDevice(SDL_VideoDevice * device)
{
    if (device->driverdata) {
        SDL_VideoData * video_data = (SDL_VideoData *)device->driverdata;
        if (video_data->winrtEglWindow) {
            video_data->winrtEglWindow->Release();
        }
        SDL_free(video_data);
    }

    SDL_free(device);
}

static SDL_VideoDevice *
WINRT_CreateDevice(int devindex)
{
    SDL_VideoDevice *device;
    SDL_VideoData *data;

    /* Initialize all variables that we clean on shutdown */
    device = (SDL_VideoDevice *) SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        if (device) {
            SDL_free(device);
        }
        return (0);
    }

    data = (SDL_VideoData *) SDL_calloc(1, sizeof(SDL_VideoData));
    if (!data) {
        SDL_OutOfMemory();
        return (0);
    }
    SDL_zerop(data);
    device->driverdata = data;

    /* Set the function pointers */
    device->VideoInit = WINRT_VideoInit;
    device->VideoQuit = WINRT_VideoQuit;
    device->CreateWindow = WINRT_CreateWindow;
    device->DestroyWindow = WINRT_DestroyWindow;
    device->SetDisplayMode = WINRT_SetDisplayMode;
    device->PumpEvents = WINRT_PumpEvents;
    device->GetWindowWMInfo = WINRT_GetWindowWMInfo;
#ifdef SDL_VIDEO_OPENGL_EGL
    device->GL_LoadLibrary = WINRT_GLES_LoadLibrary;
    device->GL_GetProcAddress = WINRT_GLES_GetProcAddress;
    device->GL_UnloadLibrary = WINRT_GLES_UnloadLibrary;
    device->GL_CreateContext = WINRT_GLES_CreateContext;
    device->GL_MakeCurrent = WINRT_GLES_MakeCurrent;
    device->GL_SetSwapInterval = WINRT_GLES_SetSwapInterval;
    device->GL_GetSwapInterval = WINRT_GLES_GetSwapInterval;
    device->GL_SwapWindow = WINRT_GLES_SwapWindow;
    device->GL_DeleteContext = WINRT_GLES_DeleteContext;
#endif
    device->free = WINRT_DeleteDevice;

    return device;
}

#define WINRTVID_DRIVER_NAME "winrt"
VideoBootStrap WINRT_bootstrap = {
    WINRTVID_DRIVER_NAME, "SDL WinRT video driver",
    WINRT_Available, WINRT_CreateDevice
};

int
WINRT_VideoInit(_THIS)
{
    if (WINRT_InitModes(_this) < 0) {
        return -1;
    }
    WINRT_InitMouse(_this);
    WINRT_InitTouch(_this);

    return 0;
}

extern "C"
Uint32 D3D11_DXGIFormatToSDLPixelFormat(DXGI_FORMAT dxgiFormat);

static void
WINRT_DXGIModeToSDLDisplayMode(const DXGI_MODE_DESC * dxgiMode, SDL_DisplayMode * sdlMode)
{
    SDL_zerop(sdlMode);
    sdlMode->w = dxgiMode->Width;
    sdlMode->h = dxgiMode->Height;
    sdlMode->refresh_rate = dxgiMode->RefreshRate.Numerator / dxgiMode->RefreshRate.Denominator;
    sdlMode->format = D3D11_DXGIFormatToSDLPixelFormat(dxgiMode->Format);
}

static int
WINRT_AddDisplaysForOutput (_THIS, IDXGIAdapter1 * dxgiAdapter1, int outputIndex)
{
    HRESULT hr;
    IDXGIOutput * dxgiOutput = NULL;
    DXGI_OUTPUT_DESC dxgiOutputDesc;
    SDL_VideoDisplay display;
    char * displayName = NULL;
    UINT numModes;
    DXGI_MODE_DESC * dxgiModes = NULL;
    int functionResult = -1;        /* -1 for failure, 0 for success */
    DXGI_MODE_DESC modeToMatch, closestMatch;

    SDL_zero(display);

    hr = dxgiAdapter1->EnumOutputs(outputIndex, &dxgiOutput);
    if (FAILED(hr)) {
        if (hr != DXGI_ERROR_NOT_FOUND) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIAdapter1::EnumOutputs failed", hr);
        }
        goto done;
    }

    hr = dxgiOutput->GetDesc(&dxgiOutputDesc);
    if (FAILED(hr)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIOutput::GetDesc failed", hr);
        goto done;
    }

    SDL_zero(modeToMatch);
    modeToMatch.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    modeToMatch.Width = (dxgiOutputDesc.DesktopCoordinates.right - dxgiOutputDesc.DesktopCoordinates.left);
    modeToMatch.Height = (dxgiOutputDesc.DesktopCoordinates.bottom - dxgiOutputDesc.DesktopCoordinates.top);
    hr = dxgiOutput->FindClosestMatchingMode(&modeToMatch, &closestMatch, NULL);
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
        /* DXGI_ERROR_NOT_CURRENTLY_AVAILABLE gets returned by IDXGIOutput::FindClosestMatchingMode
           when running under the Windows Simulator, which uses Remote Desktop (formerly known as Terminal
           Services) under the hood.  According to the MSDN docs for the similar function,
           IDXGIOutput::GetDisplayModeList, DXGI_ERROR_NOT_CURRENTLY_AVAILABLE is returned if and
           when an app is run under a Terminal Services session, hence the assumption.

           In this case, just add an SDL display mode, with approximated values.
        */
        SDL_DisplayMode mode;
        SDL_zero(mode);
        display.name = "Windows Simulator / Terminal Services Display";
        mode.w = (dxgiOutputDesc.DesktopCoordinates.right - dxgiOutputDesc.DesktopCoordinates.left);
        mode.h = (dxgiOutputDesc.DesktopCoordinates.bottom - dxgiOutputDesc.DesktopCoordinates.top);
        mode.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        mode.refresh_rate = 0;  /* Display mode is unknown, so just fill in zero, as specified by SDL's header files */
        display.desktop_mode = mode;
        display.current_mode = mode;
        if ( ! SDL_AddDisplayMode(&display, &mode)) {
            goto done;
        }
    } else if (FAILED(hr)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIOutput::FindClosestMatchingMode failed", hr);
        goto done;
    } else {
        displayName = WIN_StringToUTF8(dxgiOutputDesc.DeviceName);
        display.name = displayName;
        WINRT_DXGIModeToSDLDisplayMode(&closestMatch, &display.desktop_mode);
        display.current_mode = display.desktop_mode;

        hr = dxgiOutput->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &numModes, NULL);
        if (FAILED(hr)) {
            if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
                // TODO, WinRT: make sure display mode(s) are added when using Terminal Services / Windows Simulator
            }
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIOutput::GetDisplayModeList [get mode list size] failed", hr);
            goto done;
        }

        dxgiModes = (DXGI_MODE_DESC *)SDL_calloc(numModes, sizeof(DXGI_MODE_DESC));
        if ( ! dxgiModes) {
            SDL_OutOfMemory();
            goto done;
        }

        hr = dxgiOutput->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, 0, &numModes, dxgiModes);
        if (FAILED(hr)) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIOutput::GetDisplayModeList [get mode contents] failed", hr);
            goto done;
        }

        for (UINT i = 0; i < numModes; ++i) {
            SDL_DisplayMode sdlMode;
            WINRT_DXGIModeToSDLDisplayMode(&dxgiModes[i], &sdlMode);
            SDL_AddDisplayMode(&display, &sdlMode);
        }
    }

    if (SDL_AddVideoDisplay(&display) < 0) {
        goto done;
    }

    functionResult = 0;     /* 0 for Success! */
done:
    if (dxgiModes) {
        SDL_free(dxgiModes);
    }
    if (dxgiOutput) {
        dxgiOutput->Release();
    }
    if (displayName) {
        SDL_free(displayName);
    }
    return functionResult;
}

static int
WINRT_AddDisplaysForAdapter (_THIS, IDXGIFactory2 * dxgiFactory2, int adapterIndex)
{
    HRESULT hr;
    IDXGIAdapter1 * dxgiAdapter1;

    hr = dxgiFactory2->EnumAdapters1(adapterIndex, &dxgiAdapter1);
    if (FAILED(hr)) {
        if (hr != DXGI_ERROR_NOT_FOUND) {
            WIN_SetErrorFromHRESULT(__FUNCTION__ ", IDXGIFactory1::EnumAdapters1() failed", hr);
        }
        return -1;
    }

    for (int outputIndex = 0; ; ++outputIndex) {
        if (WINRT_AddDisplaysForOutput(_this, dxgiAdapter1, outputIndex) < 0) {
            break;
        }
    }

    dxgiAdapter1->Release();
    return 0;
}

int
WINRT_InitModes(_THIS)
{
    /* HACK: Initialize a single display, for whatever screen the app's
         CoreApplicationView is on.
       TODO, WinRT: Try initializing multiple displays, one for each monitor.
         Appropriate WinRT APIs for this seem elusive, though.  -- DavidL
    */

    HRESULT hr;
    IDXGIFactory2 * dxgiFactory2 = NULL;

    hr = CreateDXGIFactory1(IID_IDXGIFactory2, (void **)&dxgiFactory2);
    if (FAILED(hr)) {
        WIN_SetErrorFromHRESULT(__FUNCTION__ ", CreateDXGIFactory1() failed", hr);
        return -1;
    }

    int adapterIndex = 0;
    for (int adapterIndex = 0; ; ++adapterIndex) {
        if (WINRT_AddDisplaysForAdapter(_this, dxgiFactory2, adapterIndex) < 0) {
            break;
        }
    }

    return 0;
}

static int
WINRT_SetDisplayMode(_THIS, SDL_VideoDisplay * display, SDL_DisplayMode * mode)
{
    return 0;
}

void
WINRT_VideoQuit(_THIS)
{
    WINRT_QuitMouse(_this);
}

static const Uint32 WINRT_DetectableFlags =
    SDL_WINDOW_MAXIMIZED |
    SDL_WINDOW_FULLSCREEN_DESKTOP |
    SDL_WINDOW_SHOWN |
    SDL_WINDOW_HIDDEN |
    SDL_WINDOW_MOUSE_FOCUS;

extern "C" Uint32
WINRT_DetectWindowFlags(SDL_Window * window)
{
    Uint32 latestFlags = 0;
    SDL_WindowData * data = (SDL_WindowData *) window->driverdata;
    bool is_fullscreen = false;

#if SDL_WINRT_USE_APPLICATIONVIEW
    if (data->appView) {
        is_fullscreen = data->appView->IsFullScreen;
    }
#elif (WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP)
    is_fullscreen = true;
#endif

    if (data->coreWindow.Get()) {
        if (is_fullscreen) {
            SDL_VideoDisplay * display = SDL_GetDisplayForWindow(window);
            int w = WINRT_DIPS_TO_PHYSICAL_PIXELS(data->coreWindow->Bounds.Width);
            int h = WINRT_DIPS_TO_PHYSICAL_PIXELS(data->coreWindow->Bounds.Height);

#if (WINAPI_FAMILY != WINAPI_FAMILY_PHONE_APP) || (NTDDI_VERSION > NTDDI_WIN8)
            // On all WinRT platforms, except for WinPhone 8.0, rotate the
            // window size.  This is needed to properly calculate
            // fullscreen vs. maximized.
            const DisplayOrientations currentOrientation = WINRT_DISPLAY_PROPERTY(CurrentOrientation);
            switch (currentOrientation) {
#if (WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP)
                case DisplayOrientations::Landscape:
                case DisplayOrientations::LandscapeFlipped:
#else
                case DisplayOrientations::Portrait:
                case DisplayOrientations::PortraitFlipped:
#endif
                {
                    int tmp = w;
                    w = h;
                    h = tmp;
                } break;
            }
#endif

            if (display->desktop_mode.w != w || display->desktop_mode.h != h) {
                latestFlags |= SDL_WINDOW_MAXIMIZED;
            } else {
                latestFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            }
        }

        if (data->coreWindow->Visible) {
            latestFlags |= SDL_WINDOW_SHOWN;
        } else {
            latestFlags |= SDL_WINDOW_HIDDEN;
        }

#if (WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP) && (NTDDI_VERSION < NTDDI_WINBLUE)
        // data->coreWindow->PointerPosition is not supported on WinPhone 8.0
        latestFlags |= SDL_WINDOW_MOUSE_FOCUS;
#else
        if (data->coreWindow->Bounds.Contains(data->coreWindow->PointerPosition)) {
            latestFlags |= SDL_WINDOW_MOUSE_FOCUS;
        }
#endif
    }

    return latestFlags;
}

void
WINRT_UpdateWindowFlags(SDL_Window * window, Uint32 mask)
{
    mask &= WINRT_DetectableFlags;
    if (window) {
        Uint32 apply = WINRT_DetectWindowFlags(window);
        if ((apply & mask) & SDL_WINDOW_FULLSCREEN) {
            window->last_fullscreen_flags = window->flags;  // seems necessary to programmatically un-fullscreen, via SDL APIs
        }
        window->flags = (window->flags & ~mask) | (apply & mask);
    }
}

int
WINRT_CreateWindow(_THIS, SDL_Window * window)
{
    // Make sure that only one window gets created, at least until multimonitor
    // support is added.
    if (WINRT_GlobalSDLWindow != NULL) {
        SDL_SetError("WinRT only supports one window");
        return -1;
    }

    SDL_WindowData *data = new SDL_WindowData;  /* use 'new' here as SDL_WindowData may use WinRT/C++ types */
    if (!data) {
        SDL_OutOfMemory();
        return -1;
    }
    window->driverdata = data;
    data->sdlWindow = window;

    /* To note, when XAML support is enabled, access to the CoreWindow will not
       be possible, at least not via the SDL/XAML thread.  Attempts to access it
       from there will throw exceptions.  As such, the SDL_WindowData's
       'coreWindow' field will only be set (to a non-null value) if XAML isn't
       enabled.
    */
    if (!WINRT_XAMLWasEnabled) {
        data->coreWindow = CoreWindow::GetForCurrentThread();
#if SDL_WINRT_USE_APPLICATIONVIEW
        data->appView = ApplicationView::GetForCurrentView();
#endif
    }

#if SDL_VIDEO_OPENGL_EGL
    /* Setup the EGL surface, but only if OpenGL ES 2 was requested. */
    if (!(window->flags & SDL_WINDOW_OPENGL)) {
        /* OpenGL ES 2 wasn't requested.  Don't set up an EGL surface. */
        data->egl_surface = EGL_NO_SURFACE;
    } else {
        /* OpenGL ES 2 was reuqested.  Set up an EGL surface. */
        SDL_VideoData * video_data = (SDL_VideoData *)_this->driverdata;

        /* Call SDL_EGL_ChooseConfig and eglCreateWindowSurface directly,
         * rather than via SDL_EGL_CreateSurface, as older versions of
         * ANGLE/WinRT may require that a C++ object, ComPtr<IUnknown>,
         * be passed into eglCreateWindowSurface.
         */
        if (SDL_EGL_ChooseConfig(_this) != 0) {
            char buf[512];
            SDL_snprintf(buf, sizeof(buf), "SDL_EGL_ChooseConfig failed: %s", SDL_GetError());
            return SDL_SetError("%s", buf);
        }

        if (video_data->winrtEglWindow) {   /* ... is the 'old' version of ANGLE/WinRT being used? */
            /* Attempt to create a window surface using older versions of
             * ANGLE/WinRT:
             */
            Microsoft::WRL::ComPtr<IUnknown> cpp_winrtEglWindow = video_data->winrtEglWindow;
            data->egl_surface = ((eglCreateWindowSurface_Old_Function)_this->egl_data->eglCreateWindowSurface)(
                _this->egl_data->egl_display,
                _this->egl_data->egl_config,
                cpp_winrtEglWindow, NULL);
            if (data->egl_surface == NULL) {
                return SDL_SetError("eglCreateWindowSurface failed");
            }
        } else if (data->coreWindow.Get() != nullptr) {
            /* Attempt to create a window surface using newer versions of
             * ANGLE/WinRT:
             */
            IInspectable * coreWindowAsIInspectable = reinterpret_cast<IInspectable *>(data->coreWindow.Get());
            data->egl_surface = _this->egl_data->eglCreateWindowSurface(
                _this->egl_data->egl_display,
                _this->egl_data->egl_config,
                coreWindowAsIInspectable,
                NULL);
            if (data->egl_surface == NULL) {
                return SDL_SetError("eglCreateWindowSurface failed");
            }
        } else {
            return SDL_SetError("No supported means to create an EGL window surface are available");
        }
    }
#endif

#if SDL_WINRT_USE_APPLICATIONVIEW
    /* Determine as many flags dynamically, as possible. */
    window->flags =
        SDL_WINDOW_BORDERLESS;
#else
    /* Set SDL_Window flags for Windows Phone 8.0 */
    window->flags =
        SDL_WINDOW_FULLSCREEN_DESKTOP |
        SDL_WINDOW_BORDERLESS |
        SDL_WINDOW_MAXIMIZED |
        SDL_WINDOW_INPUT_GRABBED;
#endif

#if SDL_VIDEO_OPENGL_EGL
    if (data->egl_surface) {
        window->flags |= SDL_WINDOW_OPENGL;
    }
#endif

    if (WINRT_XAMLWasEnabled) {
        /* TODO, WinRT: set SDL_Window size, maybe position too, from XAML control */
        window->x = 0;
        window->y = 0;
        window->flags |= SDL_WINDOW_SHOWN;
        SDL_SetMouseFocus(NULL);        // TODO: detect this
        SDL_SetKeyboardFocus(NULL);     // TODO: detect this
    } else {
        /* WinRT apps seem to live in an environment where the OS controls the
           app's window size, with some apps being fullscreen, depending on
           user choice of various things.  For now, just adapt the SDL_Window to
           whatever Windows set-up as the native-window's geometry.
        */
        window->x = WINRT_DIPS_TO_PHYSICAL_PIXELS(data->coreWindow->Bounds.Left);
        window->y = WINRT_DIPS_TO_PHYSICAL_PIXELS(data->coreWindow->Bounds.Top);
        window->w = WINRT_DIPS_TO_PHYSICAL_PIXELS(data->coreWindow->Bounds.Width);
        window->h = WINRT_DIPS_TO_PHYSICAL_PIXELS(data->coreWindow->Bounds.Height);

        WINRT_UpdateWindowFlags(
            window,
            0xffffffff      /* Update any window flag(s) that WINRT_UpdateWindow can handle */
        );

        /* Try detecting if the window is active */
        bool isWindowActive = true;     /* Presume the window is active, unless we've been told otherwise */
        if (data->coreWindow->CustomProperties->HasKey("SDLHelperWindowActivationState")) {
            CoreWindowActivationState activationState = \
                safe_cast<CoreWindowActivationState>(data->coreWindow->CustomProperties->Lookup("SDLHelperWindowActivationState"));
            isWindowActive = (activationState != CoreWindowActivationState::Deactivated);
        }
        if (isWindowActive) {
            SDL_SetKeyboardFocus(window);
        }
    }
 
    /* Make sure the WinRT app's IFramworkView can post events on
       behalf of SDL:
    */
    WINRT_GlobalSDLWindow = window;

    /* All done! */
    return 0;
}

void
WINRT_DestroyWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData * data = (SDL_WindowData *) window->driverdata;

    if (WINRT_GlobalSDLWindow == window) {
        WINRT_GlobalSDLWindow = NULL;
    }

    if (data) {
        // Delete the internal window data:
        delete data;
        data = NULL;
        window->driverdata = NULL;
    }
}

SDL_bool
WINRT_GetWindowWMInfo(_THIS, SDL_Window * window, SDL_SysWMinfo * info)
{
    SDL_WindowData * data = (SDL_WindowData *) window->driverdata;

    if (info->version.major <= SDL_MAJOR_VERSION) {
        info->subsystem = SDL_SYSWM_WINRT;
        info->info.winrt.window = reinterpret_cast<IInspectable *>(data->coreWindow.Get());
        return SDL_TRUE;
    } else {
        SDL_SetError("Application not compiled with SDL %d.%d\n",
                     SDL_MAJOR_VERSION, SDL_MINOR_VERSION);
        return SDL_FALSE;
    }
    return SDL_FALSE;
}

#endif /* SDL_VIDEO_DRIVER_WINRT */

/* vi: set ts=4 sw=4 expandtab: */
