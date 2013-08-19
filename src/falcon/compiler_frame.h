
#ifndef FALCON_COMPILER_FRAME_H
#define FALCON_COMPILER_FRAME_H

struct Frame {
  int target;
  int stack_pos;
  bool is_exc_handler;
};

#endif
