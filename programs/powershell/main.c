/*
 * Copyright 2017 Jactry Zeng for CodeWeavers
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

#include <wchar.h>

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(powershell);

static const WCHAR *skip_spaces(const WCHAR *p)
{
    while (iswspace(*p)) ++p;
    return p;
}

static const WCHAR *skip_word(const WCHAR *p, const WCHAR *word)
{
    size_t len = wcslen(word);

    return wcsnicmp(p, word, len) ? NULL : p + len;
}

static BOOL parse_else_exit(const WCHAR *p, int *code)
{
    WCHAR *end;
    int val;

    if (!(p = skip_word(p, L"else"))) return FALSE;
    p = skip_spaces(p);
    if (*p++ != '{') return FALSE;
    if (!(p = skip_word(skip_spaces(p), L"exit")) || !iswspace(*p)) return FALSE;
    val = wcstol(p, &end, 10);
    if (end == p) return FALSE;
    p = skip_spaces(end);
    if (*p == ';') p = skip_spaces(p + 1);
    if (*p++ != '}') return FALSE;
    p = skip_spaces(p);
    if (*p == ';') p = skip_spaces(p + 1);
    if (*p) return FALSE;
    *code = val;
    return TRUE;
}

static BOOL get_command_probe_exit_code(const WCHAR *script, int *code)
{
    const WCHAR *p, *q;

    if (!(p = skip_word(skip_spaces(script), L"if"))) return FALSE;
    p = skip_spaces(p);
    if (*p != '(') return FALSE;
    while (*p == '(') p = skip_spaces(p + 1);
    if (!(p = skip_word(p, L"Get-Command")) || !iswspace(*p)) return FALSE;

    for (q = p + wcslen(p); q > p; --q)
        if ((iswspace(q[-1]) || q[-1] == '}') && parse_else_exit(q, code)) return TRUE;
    return FALSE;
}

int __cdecl wmain(int argc, WCHAR *argv[])
{
    int i, ret = 0;

    WINE_FIXME("stub.\n");
    for (i = 0; i < argc; i++)
    {
        WINE_FIXME("argv[%d] %s\n", i, wine_dbgstr_w(argv[i]));
        if (!wcsicmp(argv[i], L"-command") && i < argc - 1 && !wcscmp(argv[i + 1], L"-"))
        {
            char command[4096], *p;

            ++i;
            while (fgets(command, sizeof(command), stdin))
            {
                WINE_FIXME("command %s.\n", debugstr_a(command));
                p = command;
                while (*p && !isspace(*p)) ++p;
                *p = 0;
                if (!stricmp(command, "exit"))
                    break;
            }
        }
    }

    if (argc > 2 && (!wcsicmp(argv[argc - 2], L"-command") || !wcsicmp(argv[argc - 2], L"-c"))
            && get_command_probe_exit_code(argv[argc - 1], &ret))
        WINE_FIXME("cmdlet probe, returning %d.\n", ret);
    return ret;
}
