
#ifndef FALCON_CONFIG_H
#define FALCON_CONFIG_H

// These defines enable/disable certain optimizations in the
// evaluator:

#ifndef STACK_ALLOC_REGISTERS
#define STACK_ALLOC_REGISTERS 1
#endif

#ifndef PACK_INSTRUCTIONS
#define PACK_INSTRUCTIONS 1
#endif

#ifndef GETATTR_HINTS
#define GETATTR_HINTS 1
#endif

#ifndef REUSE_INT_REGISTERS
#define REUSE_INT_REGISTERS 0
#endif

#ifndef ENABLE_EXCEPTIONS
#define ENABLE_EXCEPTIONS 0
#endif

#ifndef USE_TYPED_REGISTERS
#define USE_TYPED_REGISTERS 0
#endif


#endif
