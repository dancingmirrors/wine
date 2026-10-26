/*
 * Copyright 2026 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * In addition to the permissions in the GNU Lesser General Public License,
 * the authors give you unlimited permission to link the compiled version
 * of this file with other programs, and to distribute those programs
 * without any restriction coming from the use of this file.  (The GNU
 * Lesser General Public License restrictions do apply in other respects;
 * for example, they cover modification of the file, and distribution when
 * not linked into another program.)
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
#pragma makedep implib
#endif

#ifdef __WINE_PE_BUILD

#include <stdarg.h>
#include <stdlib.h>
#include "../msvcrt/cxx.h"
#include "wine/asm.h"

/* The RTTI of type_info itself is never needed, so its init function goes unused. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
CREATE_TYPE_INFO_VTABLE
#pragma GCC diagnostic pop

asm( ".globl \"??_7type_info@@6B@\"\n"
     "\"??_7type_info@@6B@\"=" __ASM_NAME("type_info_vtable") "\n" );

/* list of the names cached by type_info::name(), see __std_type_info_name() */
struct type_info_node
{
    SLIST_HEADER list;
};

struct type_info_node type_info_root_node = { 0 };

asm( ".globl \"?__type_info_root_node@@3U__type_info_node@@A\"\n"
     "\"?__type_info_root_node@@3U__type_info_node@@A\"=" __ASM_NAME("type_info_root_node") "\n" );

#endif
