#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "winreg.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(steamstub);

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
