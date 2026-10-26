/*
 * Minimal C++ runtime support for the bundled ICU
 *
 * Copyright (C) the Wine project
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

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <corecrt_startup.h>
#include <new>
#include <system_error>

/* ICU and this library are built without exceptions, so allocation
 * failures that would throw std::bad_alloc abort instead. */

void *operator new( size_t size )
{
    void *ptr;

    if (!size) size = 1;
    if (!(ptr = malloc( size ))) abort();
    return ptr;
}

void *operator new[]( size_t size )
{
    return ::operator new( size );
}

void *operator new( size_t size, const std::nothrow_t & ) noexcept
{
    return malloc( size ? size : 1 );
}

void *operator new[]( size_t size, const std::nothrow_t & ) noexcept
{
    return malloc( size ? size : 1 );
}

void operator delete( void *ptr ) noexcept
{
    free( ptr );
}

void operator delete[]( void *ptr ) noexcept
{
    free( ptr );
}

void operator delete( void *ptr, size_t ) noexcept
{
    free( ptr );
}

void operator delete[]( void *ptr, size_t ) noexcept
{
    free( ptr );
}

void operator delete( void *ptr, const std::nothrow_t & ) noexcept
{
    free( ptr );
}

void operator delete[]( void *ptr, const std::nothrow_t & ) noexcept
{
    free( ptr );
}

void *operator new( size_t size, std::align_val_t align )
{
    size_t alignment = static_cast<size_t>(align);
    void *ptr;

    if (!size) size = 1;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    if (!(ptr = _aligned_malloc( size, alignment ))) abort();
    return ptr;
}

void *operator new[]( size_t size, std::align_val_t align )
{
    return ::operator new( size, align );
}

void *operator new( size_t size, std::align_val_t align, const std::nothrow_t & ) noexcept
{
    size_t alignment = static_cast<size_t>(align);

    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    return _aligned_malloc( size ? size : 1, alignment );
}

void *operator new[]( size_t size, std::align_val_t align, const std::nothrow_t &nothrow ) noexcept
{
    return ::operator new( size, align, nothrow );
}

void operator delete( void *ptr, std::align_val_t ) noexcept
{
    _aligned_free( ptr );
}

void operator delete[]( void *ptr, std::align_val_t ) noexcept
{
    _aligned_free( ptr );
}

void operator delete( void *ptr, size_t, std::align_val_t ) noexcept
{
    _aligned_free( ptr );
}

void operator delete[]( void *ptr, size_t, std::align_val_t ) noexcept
{
    _aligned_free( ptr );
}

void operator delete( void *ptr, std::align_val_t, const std::nothrow_t & ) noexcept
{
    _aligned_free( ptr );
}

void operator delete[]( void *ptr, std::align_val_t, const std::nothrow_t & ) noexcept
{
    _aligned_free( ptr );
}

/* Destructors of function-local statics are registered with atexit(). They
 * go into a table of our own, which the dll entry point runs on unload. */
static _onexit_table_t atexit_table;

extern "C" int __cdecl atexit( void (__cdecl *func)(void) )
{
    return _register_onexit_function( &atexit_table, (_onexit_t)func );
}

extern "C" void __cdecl __wine_icu_cxx_exit(void)
{
    _execute_onexit_table( &atexit_table );
}

_LIBCPP_BEGIN_NAMESPACE_STD

/* normally in libc++'s system_error.cpp, which pulls in a lot more */
void __throw_system_error( int ev, const char *what_arg )
{
    fprintf( stderr, "system_error was thrown in -fno-exceptions mode with error %i and message \"%s\"\n",
             ev, what_arg );
    abort();
}

_LIBCPP_END_NAMESPACE_STD
