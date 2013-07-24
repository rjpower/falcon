#ifndef PY_INCLUDE_H
#define PY_INCLUDE_H

// Python.h insists on redefining a bunch of macros, giving
// lots of spurious warnings.  We undef them and redef them
// here to get around that.

#ifdef _POSIX_C_SOURCE
#define __P_C_S _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#else
#define __P_C_S -1
#endif

#ifdef _XOPEN_SOURCE
#define __X_S _XOPEN_SOURCE
#undef _XOPEN_SOURCE
#else
#define __X_S -1
#endif

#include <Python.h>

#ifdef _XOPEN_SOURCE
#undef _XOPEN_SOURCE
#endif

#if __X_S != -1
#define _XOPEN_SOURCE __X_S
#endif

#ifdef _POSIX_C_SOURCE
#undef _POSIX_C_SOURCE
#endif

#if __P_C_S != -1
#define _POSIX_C_SOURCE __P_C_S
#endif


#endif /* PY_INCLUDE_H */
