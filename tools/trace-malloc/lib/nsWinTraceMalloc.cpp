/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 *
 * ***** BEGIN LICENSE BLOCK *****
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
 * The Original Code is the Windows port of trace-malloc.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2001
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Michael Judge (original author)
 *   L. David Baron <dbaron@dbaron.org>, Mozilla Corporation
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
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

#include <stdio.h>
#include <stdlib.h>

#include "prtypes.h"
#include "prlock.h"
#include "nscore.h"
#include "nsAutoLock.h"

#include "nsDebugHelpWin32.h"
#include "nsTraceMallocCallbacks.h"

/***************************************************************************/
// shows how to use the dhw stuff to hook imported functions

#if _MSC_VER < 1300
#define NS_DEBUG_CRT "MSVCRTD.dll"
#elif _MSC_VER == 1300
#define NS_DEBUG_CRT "msvcr70d.dll"
#elif _MSC_VER == 1310
#define NS_DEBUG_CRT "msvcr71d.dll"
#elif _MSC_VER == 1400
#define NS_DEBUG_CRT "msvcr80d.dll"
#elif _MSC_VER == 1500
#define NS_DEBUG_CRT "msvcr90d.dll"
#elif _MSC_VER == 1600
#define NS_DEBUG_CRT "msvcr100d.dll"
#else
#error "Don't know filename of MSVC debug library."
#endif

DHW_DECLARE_FUN_TYPE_AND_PROTO(dhw_malloc, void*, __cdecl, MALLOC_, (size_t));

DHWImportHooker &getMallocHooker()
{
  static DHWImportHooker gMallocHooker(NS_DEBUG_CRT, "malloc", (PROC) dhw_malloc);
  return gMallocHooker;
}

void * __cdecl dhw_malloc( size_t size )
{
    tm_thread *t = tm_get_thread();
    ++t->suppress_tracing;
    PRUint32 start = PR_IntervalNow();
    void* result = DHW_ORIGINAL(MALLOC_, getMallocHooker())(size);
    PRUint32 end = PR_IntervalNow();
    --t->suppress_tracing;
    MallocCallback(result, size, start, end, t);
    return result;    
}

DHW_DECLARE_FUN_TYPE_AND_PROTO(dhw_calloc, void*, __cdecl, CALLOC_, (size_t,size_t));

DHWImportHooker &getCallocHooker()
{
  static DHWImportHooker gCallocHooker(NS_DEBUG_CRT, "calloc", (PROC) dhw_calloc);
  return gCallocHooker;
}

void * __cdecl dhw_calloc( size_t count, size_t size )
{
    tm_thread *t = tm_get_thread();
    ++t->suppress_tracing;
    PRUint32 start = PR_IntervalNow();
    void* result = DHW_ORIGINAL(CALLOC_, getCallocHooker())(count,size);
    PRUint32 end = PR_IntervalNow();
    --t->suppress_tracing;
    CallocCallback(result, count, size, start, end, t);
    return result;    
}

DHW_DECLARE_FUN_TYPE_AND_PROTO(dhw_free, void, __cdecl, FREE_, (void*));
DHWImportHooker &getFreeHooker()
{
  static DHWImportHooker gFreeHooker(NS_DEBUG_CRT, "free", (PROC) dhw_free);
  return gFreeHooker;
}

void __cdecl dhw_free( void* p )
{
    tm_thread *t = tm_get_thread();
    ++t->suppress_tracing;
    PRUint32 start = PR_IntervalNow();
    DHW_ORIGINAL(FREE_, getFreeHooker())(p);
    PRUint32 end = PR_IntervalNow();
    --t->suppress_tracing;
    /* FIXME bug 392008: We could race with reallocation of p. */
    FreeCallback(p, start, end, t);
}


DHW_DECLARE_FUN_TYPE_AND_PROTO(dhw_realloc, void*, __cdecl, REALLOC_, (void*, size_t));
DHWImportHooker &getReallocHooker()
{
  static DHWImportHooker gReallocHooker(NS_DEBUG_CRT, "realloc", (PROC) dhw_realloc);
  return gReallocHooker;
}

void * __cdecl dhw_realloc(void * pin, size_t size)
{
    tm_thread *t = tm_get_thread();
    ++t->suppress_tracing;
    PRUint32 start = PR_IntervalNow();
    void* pout = DHW_ORIGINAL(REALLOC_, getReallocHooker())(pin, size);
    PRUint32 end = PR_IntervalNow();
    --t->suppress_tracing;
    /* FIXME bug 392008: We could race with reallocation of pin. */
    ReallocCallback(pin, pout, size, start, end, t);
    return pout;
}

// Note the mangled name!
DHW_DECLARE_FUN_TYPE_AND_PROTO(dhw_new, void*, __cdecl, NEW_, (size_t));
DHWImportHooker &getNewHooker()
{
  static DHWImportHooker gNewHooker(NS_DEBUG_CRT, "??2@YAPAXI@Z", (PROC) dhw_new);
  return gNewHooker;
}

void * __cdecl dhw_new(size_t size)
{
    tm_thread *t = tm_get_thread();
    ++t->suppress_tracing;
    PRUint32 start = PR_IntervalNow();
    void* result = DHW_ORIGINAL(NEW_, getNewHooker())(size);
    PRUint32 end = PR_IntervalNow();
    --t->suppress_tracing;
    MallocCallback(result, size, start, end, t);//do we need a different one for new?
    return result;
}

// Note the mangled name!
DHW_DECLARE_FUN_TYPE_AND_PROTO(dhw_delete, void, __cdecl, DELETE_, (void*));
DHWImportHooker &getDeleteHooker()
{
  static DHWImportHooker gDeleteHooker(NS_DEBUG_CRT, "??3@YAXPAX@Z", (PROC) dhw_delete);
  return gDeleteHooker;
}

void __cdecl dhw_delete(void* p)
{
    tm_thread *t = tm_get_thread();
    ++t->suppress_tracing;
    PRUint32 start = PR_IntervalNow();
    DHW_ORIGINAL(DELETE_, getDeleteHooker())(p);
    PRUint32 end = PR_IntervalNow();
    --t->suppress_tracing;
    FreeCallback(p, start, end, t);
}

// Note the mangled name!
DHW_DECLARE_FUN_TYPE_AND_PROTO(dhw_vec_new, void*, __cdecl, VEC_NEW_, (size_t));
DHWImportHooker &getVecNewHooker()
{
  static DHWImportHooker gVecNewHooker(NS_DEBUG_CRT, "??_U@YAPAXI@Z", (PROC) dhw_vec_new);
  return gVecNewHooker;
}

void * __cdecl dhw_vec_new(size_t size)
{
    tm_thread *t = tm_get_thread();
    ++t->suppress_tracing; // need to suppress since new[] calls new
    PRUint32 start = PR_IntervalNow();
    void* result = DHW_ORIGINAL(VEC_NEW_, getVecNewHooker())(size);
    PRUint32 end = PR_IntervalNow();
    --t->suppress_tracing;
    MallocCallback(result, size, start, end, t);//do we need a different one for new[]?
    return result;
}

// Note the mangled name!
DHW_DECLARE_FUN_TYPE_AND_PROTO(dhw_vec_delete, void, __cdecl, VEC_DELETE_, (void*));
DHWImportHooker &getVecDeleteHooker()
{
  static DHWImportHooker gVecDeleteHooker(NS_DEBUG_CRT, "??_V@YAXPAX@Z", (PROC) dhw_vec_delete);
  return gVecDeleteHooker;
}

void __cdecl dhw_vec_delete(void* p)
{
    tm_thread *t = tm_get_thread();
    ++t->suppress_tracing;
    PRUint32 start = PR_IntervalNow();
    DHW_ORIGINAL(VEC_DELETE_, getVecDeleteHooker())(p);
    PRUint32 end = PR_IntervalNow();
    --t->suppress_tracing;
    FreeCallback(p, start, end, t);
}

/*C Callbacks*/
PR_IMPLEMENT(void)
StartupHooker()
{
  //run through get all hookers
  DHWImportHooker &loadlibraryW = DHWImportHooker::getLoadLibraryWHooker();
  DHWImportHooker &loadlibraryExW = DHWImportHooker::getLoadLibraryExWHooker();
  DHWImportHooker &loadlibraryA = DHWImportHooker::getLoadLibraryAHooker();
  DHWImportHooker &loadlibraryExA = DHWImportHooker::getLoadLibraryExAHooker();
  DHWImportHooker &mallochooker = getMallocHooker();
  DHWImportHooker &reallochooker = getReallocHooker();
  DHWImportHooker &callochooker = getCallocHooker();
  DHWImportHooker &freehooker = getFreeHooker();
  DHWImportHooker &newhooker = getNewHooker();
  DHWImportHooker &deletehooker = getDeleteHooker();
  DHWImportHooker &vecnewhooker = getVecNewHooker();
  DHWImportHooker &vecdeletehooker = getVecDeleteHooker();
  printf("Startup Hooker\n");
}

PR_IMPLEMENT(void)
ShutdownHooker()
{
}

extern "C" {
  void* dhw_orig_malloc(size_t);
  void* dhw_orig_calloc(size_t, size_t);
  void* dhw_orig_realloc(void*, size_t);
  void dhw_orig_free(void*);
}

void*
dhw_orig_malloc(size_t size)
{
    return DHW_ORIGINAL(MALLOC_, getMallocHooker())(size);
}

void*
dhw_orig_calloc(size_t count, size_t size)
{
    return DHW_ORIGINAL(CALLOC_, getCallocHooker())(count,size);
}

void*
dhw_orig_realloc(void* pin, size_t size)
{
    return DHW_ORIGINAL(REALLOC_, getReallocHooker())(pin, size);
}

void
dhw_orig_free(void* p)
{
    DHW_ORIGINAL(FREE_, getFreeHooker())(p);
}
