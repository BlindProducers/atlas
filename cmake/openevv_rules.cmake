# Writes one OpenEVV language's rule code into the build tree, run with -P by
# the custom command in cmake/openevv.cmake.
#
#   -DPYTHON=<python3> -DSOURCE=<third_party/openevv> -DTAG=<enus> -DOUT=<dir>
#
# OpenEVV keeps its rules as text in lang/<tag>/rules and every build writes
# the bytecode, its header and a C shim per rule out of that text (its
# Makefile's rulecode step). Those three files land in lang/<tag>, where
# OpenEVV's .gitignore covers them, and are copied here; the shim is patched on
# the way (see below), so the submodule's own files are never edited.

foreach(variable PYTHON SOURCE TAG OUT)
  if(NOT DEFINED ${variable})
    message(FATAL_ERROR "openevv_rules.cmake needs -D${variable}=")
  endif()
endforeach()

function(run_python)
  execute_process(COMMAND ${ARGN}
    WORKING_DIRECTORY "${SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_QUIET
    ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "OpenEVV rule generation failed (${TAG}): ${ARGN}\n${errors}")
  endif()
endfunction()

# What each entry's arguments are, read from the declarations under src.
run_python("${PYTHON}" tools/rules/entrysig.py record "lang/${TAG}")
# A language with a letters file writes its letter rules out of it first.
if(EXISTS "${SOURCE}/lang/${TAG}/letters")
  run_python("${PYTHON}" tools/rules/letters.py write "${TAG}")
endif()
run_python("${CMAKE_COMMAND}" -E env "EVV_NOTATION_LANG=${TAG}" "${PYTHON}" tools/rules/notation.py build)

file(MAKE_DIRECTORY "${OUT}")
foreach(suffix ".c" ".h")
  file(COPY_FILE "${SOURCE}/lang/${TAG}/delta_rules_${TAG}${suffix}" "${OUT}/delta_rules_${TAG}${suffix}")
endforeach()

# The interpreter calls every rule through a function pointer whose arguments
# are whole machine words (call_entry in src/delta/delta_rules.c). The shims
# take int32_t, which agrees with that on Linux, where every stack argument
# fills an eight-byte slot, but not on Apple's arm64, where a stack argument is
# packed to its own size: a rule's ninth and tenth arguments were read from
# the wrong half of a slot and the engine faulted on its first instance. So
# each shim takes uintptr_t and narrows.
file(READ "${SOURCE}/lang/${TAG}/delta_rules_shim_${TAG}.c" shim)
string(REGEX REPLACE "int32_t a([0-9]+)([,)])" "uintptr_t a\\1\\2" patched "${shim}")
string(REGEX REPLACE "= a([0-9]+);" "= (int32_t)a\\1;" patched "${patched}")
if(patched STREQUAL shim OR NOT patched MATCHES "uintptr_t a0")
  message(FATAL_ERROR "OpenEVV's rule shim for ${TAG} no longer has the shape Atlas patches")
endif()
file(WRITE "${OUT}/delta_rules_shim_${TAG}.c"
     "/* Written by cmake/openevv_rules.cmake: OpenEVV's shim with whole-word arguments. */\n"
     "#include <stdint.h>\n${patched}")
