/* **********************************************************
 * Copyright (c) 2006-2008 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "tools.h"
#include <setjmp.h>

#ifdef LINUX
# include <unistd.h>
# include <signal.h>
# include <ucontext.h>
# include <errno.h>
#endif

jmp_buf mark;
int where; /* 0 = normal, 1 = segfault longjmp */

int
ring(int num)
{
    int addr;
    print("looking at ring "PFX"\n", num);
    *(int*) (&num - 1) = num;
    return num;
}

int
twofoo()
{
    int a = foo();
    print("first foo a=%d\n", a);

    a += foo();
    print("second foo a=%d\n", a);
    return a;
}

int
foo()
{
    print("in foo\n");
    return 1;
}

int
bar()
{
    print("in bar\n");
    return 3;
}


#ifdef LINUX
/* just use single-arg handlers */
typedef void (*handler_t)(int);
typedef void (*handler_3_t)(int, struct siginfo *, void *);

static void
signal_handler(int sig)
{
    if (sig == SIGSEGV) {
#if VERY_VERBOSE
        print("Got seg fault\n");
#endif
        longjmp(mark, 1);
    }
    exit(-1);
}

#define ASSERT_NOERR(rc) do {                                   \
  if (rc) {                                                     \
     fprintf(stderr, "%s:%d rc=%d errno=%d %s\n",               \
             __FILE__, __LINE__,                                \
             rc, errno, strerror(errno));                       \
  }                                                             \
} while (0);

/* set up signal_handler as the handler for signal "sig" */
static void
intercept_signal(int sig, handler_t handler)
{
    int rc;
    struct sigaction act;

    act.sa_sigaction = (handler_3_t) handler;
    /* FIXME: due to DR bug 840 we cannot block ourself in the handler
     * since the handler does not end in a sigreturn, so we have an empty mask
     * and we use SA_NOMASK
     */
    rc = sigemptyset(&act.sa_mask); /* block no signals within handler */
    ASSERT_NOERR(rc);
    /* FIXME: due to DR bug #654 we use SA_SIGINFO -- change it once DR works */
    act.sa_flags = SA_NOMASK | SA_SIGINFO | SA_ONSTACK;
    
    /* arm the signal */
    rc = sigaction(sig, &act, NULL);
    ASSERT_NOERR(rc);
}

#else
/* sort of a hack to avoid the MessageBox of the unhandled exception spoiling
 * our batch runs
 */
# include <windows.h>
/* top-level exception handler */
static LONG
our_top_handler(struct _EXCEPTION_POINTERS * pExceptionInfo)
{
    if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
#if VERY_VERBOSE
        print("Got segfault\n");
#endif
        longjmp(mark, 1);
    }
# if VERBOSE
    print("Exception occurred, process about to die silently\n");
# endif
    return EXCEPTION_EXECUTE_HANDLER; /* => global unwind and silent death */
}
#endif

int
invalid_ret(int num) 
{
    where = setjmp(mark);
    if (where == 0) {
        ring(num);
        print("unexpectedly we came back!");
    } else {
        print("fault caught on "PFX"\n", num);
    }
}

int
main()
{
    INIT();

#ifdef LINUX
    intercept_signal(SIGSEGV, signal_handler);
#else
    SetUnhandledExceptionFilter((LPTOP_LEVEL_EXCEPTION_FILTER) our_top_handler);
#endif

    print("starting good function\n");
    twofoo();
    print("starting bad function\n");

    invalid_ret(1);                    /* zero page */
    /* FIXME: should wrap all of these in setjmp() blocks */
    invalid_ret(0);                    /* NULL */
    invalid_ret(0x00badbad);           /* user mode */
    invalid_ret(0x7fffffff);           /* user mode */
    invalid_ret(0x80000000);           /* kernel addr */
    invalid_ret(0xbadbad00);           /* kernel addr */
    invalid_ret(0xfffffffe);           /* just bad */
    invalid_ret(0xffffffff);           /* just bad */

    print("all done [not seen]\n");
}
