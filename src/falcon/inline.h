#ifndef FALCON_INLINE_H
#define FALCON_INLINE_H

/* inlining attributes */

#if defined(SWIG)

#define f_inline
#define n_inline

#elif defined(FALCON_DEBUG)

#define f_inline __attribute__((noinline))
#define n_inline __attribute__((noinline))

#else

//#define f_inline inline
//#define f_inline __attribute__((noinline))
#define f_inline __attribute__((always_inline))
#define n_inline __attribute__((noinline))

#endif

#endif
