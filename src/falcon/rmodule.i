%module falcon_core


%{
#include "rinst.h"
#include "rcompile.h"
#include "reval.h"
%}

%typemap(in) PyCodeObject* {
  if (!PyCode_Check($input)) {
    PyErr_SetString(PyExc_ValueError,"Expected a code object.");
    return NULL;
  }

  $1 = ((PyCodeObject*)$input);
}

%exception {
  try {
    $function
  } catch (RException& e) {
    e.set_python_err();
    return NULL;
  }
}

%include <std_string.i>
%include <std_map.i>
%include <std_vector.i>

%include "rinst.h"
%include "rcompile.h"
%include "reval.h"

%template(CodeVector) std::vector<CompilerOp*>;
%template(BlockVector) std::vector<BasicBlock*>;
%template(RegVector) std::vector<Register>;

%pythoncode %{
def disown_class(c):
  old_init = c.__init__
  def new_init(self, *args):
    old_init(self, *args)
    self.thisown = 0
  c.__init__ = new_init

disown_class(CompilerOp)
 
%}
