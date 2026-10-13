#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winreg.h"
#include "winternl.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(steamstub);

#ifndef DIRECTORY_QUERY
#define DIRECTORY_QUERY 0x0001
#endif

static HANDLE find_ack_event(void)
{
    static const WCHAR prefixW[] = L"STEAM_START_ACK_EVENT";
    UNICODE_STRING str = RTL_CONSTANT_STRING( L"\\BaseNamedObjects\\Session\\1" );
    DIRECTORY_BASIC_INFORMATION *di;
    OBJECT_ATTRIBUTES attr;
    HANDLE dir, ret = NULL;
    ULONG context, size;
    char buffer[1024];
    NTSTATUS status;

    di = (DIRECTORY_BASIC_INFORMATION *)buffer;
    InitializeObjectAttributes( &attr, &str, 0, 0, NULL );
    if ((status = NtOpenDirectoryObject( &dir, DIRECTORY_QUERY, &attr )))
    {
        WARN( "failed to open the session directory, status %#lx\n", status );
        return NULL;
    }

    status = NtQueryDirectoryObject( dir, di, sizeof(buffer), TRUE, TRUE, &context, &size );
    while (!status)
    {
        if (!wcsncmp( di->ObjectName.Buffer, prefixW, ARRAY_SIZE(prefixW) - 1 ))
        {
            TRACE( "found %s\n", debugstr_w(di->ObjectName.Buffer) );
            if (!(ret = OpenEventW( SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, di->ObjectName.Buffer )))
                WARN( "could not open %s, error %lu\n", debugstr_w(di->ObjectName.Buffer), GetLastError() );
            break;
        }
        status = NtQueryDirectoryObject( dir, di, sizeof(buffer), TRUE, FALSE, &context, &size );
    }
    NtClose( dir );
    return ret;
}

struct drm_ipc
{
    HANDLE consume;
    HANDLE produce;
};

static DWORD WINAPI steam_drm_thread( void *arg )
{
    struct drm_ipc *ipc = arg;
    HANDLE start_ack = NULL;

    while (WaitForSingleObject( ipc->consume, INFINITE ) == WAIT_OBJECT_0)
    {
        TRACE( "got a Steam DRM request\n" );
        if (!start_ack) start_ack = find_ack_event();
        if (start_ack) SetEvent( start_ack );
        ReleaseSemaphore( ipc->produce, 1, NULL );
    }
    return 0;
}

static DWORD WINAPI steam_windows_thread( void *arg )
{
    static WNDCLASSEXW wndclass = { sizeof(WNDCLASSEXW) };
    MSG msg;

    wndclass.lpfnWndProc = DefWindowProcW;
    wndclass.lpszClassName = L"vguiPopupWindow";
    RegisterClassExW( &wndclass );

    CreateWindowW( wndclass.lpszClassName, L"Steam", WS_POPUP,
                   40, 40, 400, 300, NULL, NULL, NULL, NULL );
    CreateWindowA( "static", "SteamVR Status", WS_POPUP,
                   0, 0, 0, 0, NULL, NULL, NULL, NULL );

    while (GetMessageW( &msg, NULL, 0, 0 ))
    {
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
    return 0;
}

static WCHAR *skip_argv0( WCHAR *cmd )
{
    BOOL quoted = FALSE;

    while (*cmd)
    {
        if (*cmd == '"') quoted = !quoted;
        else if (!quoted && (*cmd == ' ' || *cmd == '\t')) break;
        cmd++;
    }
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    return cmd;
}

int __cdecl wmain( int argc, WCHAR *argv[] )
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    WCHAR path[MAX_PATH], *p, *cmdline, *child;
    DWORD pid = GetCurrentProcessId(), code = 0;
    static struct drm_ipc ipc;
    HANDLE thread;

    if (argc < 2)
    {
        ERR( "usage: steamstub.exe <program> [args...]\n" );
        return 2;
    }

    CreateEventW( NULL, FALSE, FALSE, L"Steam3Master_SharedMemLock" );
    CreateEventW( NULL, FALSE, FALSE, L"Global\\Valve_SteamIPC_Class" );

    GetDesktopWindow();
    if ((thread = CreateThread( NULL, 0, steam_windows_thread, NULL, 0, NULL )))
        CloseHandle( thread );

    if (RegSetKeyValueW( HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess",
                         L"pid", REG_DWORD, &pid, sizeof(pid) ))
        WARN( "could not set ActiveProcess\\pid\n" );

    SetEnvironmentVariableW( L"SteamPath", L"C:\\Program Files (x86)\\Steam" );

    *path = 0;
    GetModuleFileNameW( NULL, path, MAX_PATH );
    for (p = path; *p; p++) if (*p == '\\') *p = '/';
    SetEnvironmentVariableW( L"ValvePlatformMutex", path );

    /* the misspelled name is the one Steam uses */
    ipc.consume = CreateSemaphoreW( NULL, 0, 512, L"STEAM_DIPC_CONSUME" );
    ipc.produce = CreateSemaphoreW( NULL, 1, 512, L"SREAM_DIPC_PRODUCE" );
    if (ipc.consume && ipc.produce && (thread = CreateThread( NULL, 0, steam_drm_thread, &ipc, 0, NULL )))
        CloseHandle( thread );
    else
        WARN( "could not set up Steam DRM IPC, error %lu\n", GetLastError() );

    if (!(cmdline = _wcsdup( GetCommandLineW() ))) return 1;
    child = skip_argv0( cmdline );

    TRACE( "launching %s\n", debugstr_w(child) );

    if (!CreateProcessW( NULL, child, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi ))
    {
        ERR( "cannot start %s, error %lu\n", debugstr_w(child), GetLastError() );
        free( cmdline );
        return 1;
    }

    WaitForSingleObject( pi.hProcess, INFINITE );
    GetExitCodeProcess( pi.hProcess, &code );
    CloseHandle( pi.hProcess );
    CloseHandle( pi.hThread );
    free( cmdline );
    return code;
}
