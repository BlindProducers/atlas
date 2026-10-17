# OpenEVV, the text-to-speech engine behind the Speech oscillator type.
#
# third_party/openevv is an opt-in submodule (update = none), because its
# language data is IBM's and ETI's and is not licensed for redistribution (see
# its NOTICE and src/synthesis/effects/openevv/README.atlas.md). Fetch it with
#
#   git submodule update --init --checkout third_party/openevv
#
# and configure again. Without it, or with MSVC (OpenEVV's C is GNU C), the
# Speech type is still there and stays silent.
#
# This builds what OpenEVV's own Makefile builds with RULES=bytecode: the
# engine in src, each language module in lang/<tag>, and the rule code written
# out of lang/<tag>/rules by its Python tools. Three sources are patched on the
# way into the build tree (the submodule is left as it is):
#
# - src/eci/bridge/eci_deltainit2.c: setNonSequential takes its tokens as fixed
#   arguments instead of a variable list, which Apple's arm64 passes on the
#   stack where the interpreter's call leaves them in registers.
# - src/klatt/klatt_synth.c: every parameter frame is also handed to
#   atlas_openevv_klatt_frame, which records the frames a render makes.
# - lib/eci_api.c: no constructor or destructor; Atlas starts the engine the
#   first time it renders.
#
# And each rule shim takes whole words (cmake/openevv_rules.cmake).

set(ATLAS_OPENEVV_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/openevv" CACHE PATH "OpenEVV checkout")
# Language modules built in, by OpenEVV's tag. The Speech type's Language menu
# lists all of these; a build without one speaks the first available instead.
set(ATLAS_OPENEVV_LANGUAGES enus engb dede eses esus frfr frca itit fifi ptbr CACHE STRING
    "OpenEVV language modules to build")

set(ATLAS_OPENEVV_DEFAULT OFF)
if(EXISTS "${ATLAS_OPENEVV_SOURCE}/src/delta/delta.c" AND NOT MSVC)
  set(ATLAS_OPENEVV_DEFAULT ON)
endif()
option(ATLAS_OPENEVV "Build the OpenEVV speech engine into the Speech oscillator" ${ATLAS_OPENEVV_DEFAULT})

if(NOT ATLAS_OPENEVV)
  if(NOT EXISTS "${ATLAS_OPENEVV_SOURCE}/src/delta/delta.c")
    message(STATUS "Speech oscillator without OpenEVV: git submodule update --init --checkout third_party/openevv")
  endif()
  return()
endif()
if(MSVC)
  message(FATAL_ERROR "OpenEVV is GNU C and does not build with MSVC; configure with -DATLAS_OPENEVV=OFF")
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(openevv "${ATLAS_OPENEVV_SOURCE}")
set(openevv_out "${CMAKE_CURRENT_BINARY_DIR}/openevv")
file(MAKE_DIRECTORY "${openevv_out}/patched")

# Writes a generated file only when its text changes, so a configure does not
# rebuild the engine.
function(atlas_openevv_write output text)
  file(WRITE "${output}.new" "${text}")
  file(COPY_FILE "${output}.new" "${output}" ONLY_IF_DIFFERENT)
  file(REMOVE "${output}.new")
endfunction()

# A source patched by exact replacement; a change upstream that moves the text
# stops the configure rather than building an unpatched engine.
function(atlas_openevv_patch source output)
  file(READ "${openevv}/${source}" text)
  set(patched "${text}")
  math(EXPR last "${ARGC} - 1")
  foreach(index RANGE 2 ${last} 2)
    math(EXPR next "${index} + 1")
    set(old "${ARGV${index}}")
    set(new "${ARGV${next}}")
    string(FIND "${patched}" "${old}" at)
    if(at EQUAL -1)
      message(FATAL_ERROR "OpenEVV's ${source} no longer has the text Atlas patches:\n${old}")
    endif()
    string(REPLACE "${old}" "${new}" patched "${patched}")
  endforeach()
  atlas_openevv_write("${output}" "${patched}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${openevv}/${source}")
endfunction()

atlas_openevv_patch(src/eci/bridge/eci_deltainit2.c "${openevv_out}/patched/eci_deltainit2.c"
"int32_t setNonSequential(delta_state *d, const void *count_tok, ...)
{
    va_list      ap;
    const void  *one;
    int32_t      i, left;
"
"/* Atlas: the tokens as fixed whole-word arguments. The interpreter calls
   every entry through a fixed-argument pointer, and on Apple's arm64 a
   variadic callee reads its arguments from the stack where that call left
   them in registers. Twenty-two is more than any rule passes. */
int32_t setNonSequential(delta_state *d, const void *count_tok,
                         uintptr_t t0, uintptr_t t1, uintptr_t t2,
                         uintptr_t t3, uintptr_t t4, uintptr_t t5,
                         uintptr_t t6, uintptr_t t7, uintptr_t t8,
                         uintptr_t t9, uintptr_t t10, uintptr_t t11,
                         uintptr_t t12, uintptr_t t13, uintptr_t t14,
                         uintptr_t t15, uintptr_t t16, uintptr_t t17,
                         uintptr_t t18, uintptr_t t19, uintptr_t t20,
                         uintptr_t t21)
{
    const uintptr_t toks[22] = { t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10,
                                 t11, t12, t13, t14, t15, t16, t17, t18, t19,
                                 t20, t21 };
    int32_t      next = 0;
    const void  *one;
    int32_t      i, left;
"
"    va_start(ap, count_tok);
    one = EVV_AT(const void *, va_arg(ap, int32_t));
"
"    one = EVV_AT(const void *, (int32_t)toks[next++]);
"
"        one = EVV_AT(const void *, va_arg(ap, int32_t));
        left--;
    }

    va_end(ap);"
"        one = next < 22 ? EVV_AT(const void *, (int32_t)toks[next++]) : 0;
        left--;
    }
")

atlas_openevv_patch(src/klatt/klatt_synth.c "${openevv_out}/patched/klatt_synth.c"
"#include \"evv_klatttap.h\"
"
"#include \"evv_klatttap.h\"

/* Atlas: src/synthesis/effects/openevv/atlas_openevv.c records the frames. */
void atlas_openevv_klatt_frame(const void *handle, const int32_t *parms);
"
"    evv_klatt_tap(parms);
"
"    evv_klatt_tap(parms);
    atlas_openevv_klatt_frame(handle, parms);
")

atlas_openevv_patch(lib/eci_api.c "${openevv_out}/patched/eci_api.c"
"__attribute__((constructor)) static void eci_attach(void)"
"/* Atlas starts the engine itself (atlas_openevv_start). */
static void eci_attach(void)"
"__attribute__((destructor)) static void eci_detach(void)"
"static void eci_detach(void)")

# The rule code of each language, written by OpenEVV's Python tools.
file(GLOB openevv_tools CONFIGURE_DEPENDS "${openevv}/tools/rules/*.py" "${openevv}/tools/evv.py")
set(openevv_rule_sources)
set(openevv_language_sources)
set(openevv_language_includes)
foreach(tag IN LISTS ATLAS_OPENEVV_LANGUAGES)
  if(NOT EXISTS "${openevv}/lang/${tag}/rules")
    message(FATAL_ERROR "OpenEVV has no language module lang/${tag} with rules as text")
  endif()
  file(GLOB rule_text CONFIGURE_DEPENDS "${openevv}/lang/${tag}/rules/*" "${openevv}/lang/${tag}/letters")
  list(FILTER rule_text EXCLUDE REGEX "(t_phone\\.up|constants\\.letters|\\.declared)$")
  set(outputs "${openevv_out}/${tag}/delta_rules_${tag}.c" "${openevv_out}/${tag}/delta_rules_${tag}.h"
              "${openevv_out}/${tag}/delta_rules_shim_${tag}.c")
  add_custom_command(
    OUTPUT ${outputs}
    COMMAND "${CMAKE_COMMAND}" "-DPYTHON=${Python3_EXECUTABLE}" "-DSOURCE=${openevv}" "-DTAG=${tag}"
      "-DOUT=${openevv_out}/${tag}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/openevv_rules.cmake"
    DEPENDS ${rule_text} ${openevv_tools} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/openevv_rules.cmake"
    COMMENT "Writing OpenEVV's ${tag} rules"
    VERBATIM)
  list(APPEND openevv_rule_sources "${openevv_out}/${tag}/delta_rules_${tag}.c"
                                   "${openevv_out}/${tag}/delta_rules_shim_${tag}.c")
  # The module's tables, and the empty table that leaves every rule as bytecode.
  file(GLOB module_sources CONFIGURE_DEPENDS "${openevv}/lang/${tag}/*_${tag}.c")
  list(FILTER module_sources EXCLUDE REGEX "delta_rules_(c[0-9]+_|shim_)?${tag}\\.c$")
  list(APPEND openevv_language_sources ${module_sources})
  list(APPEND openevv_language_includes "${openevv_out}/${tag}" "${openevv}/lang/${tag}")
endforeach()

# Which languages the engine has, as OpenEVV's Makefile writes it.
set(language_list "/* Written by cmake/openevv.cmake. */\n#include \"delta_lang.h\"\n\n")
set(language_table "")
set(language_binds "")
foreach(tag IN LISTS ATLAS_OPENEVV_LANGUAGES)
  string(APPEND language_list "extern delta_language delta_lang_${tag};\nvoid delta_lang_bind_${tag}(void);\n")
  string(APPEND language_table "    &delta_lang_${tag},\n")
  string(APPEND language_binds "    delta_lang_bind_${tag}();\n")
endforeach()
string(APPEND language_list "\nconst delta_language *const delta_languages[] = {\n${language_table}    0,\n};\n\n"
                            "void delta_lang_bind_all(void)\n{\n${language_binds}}\n")
atlas_openevv_write("${openevv_out}/delta_langs.c" "${language_list}")

# The engine: every source under src but the Windows port and the three
# patched above, which come from the build tree.
file(GLOB_RECURSE openevv_engine_sources CONFIGURE_DEPENDS "${openevv}/src/*.c")
list(FILTER openevv_engine_sources EXCLUDE REGEX "/(port_win32|eci_deltainit2|klatt_synth)\\.c$")
file(GLOB_RECURSE openevv_headers LIST_DIRECTORIES true "${openevv}/src/*")
set(openevv_source_dirs "${openevv}/src")
foreach(entry IN LISTS openevv_headers)
  if(IS_DIRECTORY "${entry}")
    list(APPEND openevv_source_dirs "${entry}")
  endif()
endforeach()

add_library(atlas_openevv STATIC
  ${openevv_engine_sources}
  "${openevv_out}/patched/eci_deltainit2.c"
  "${openevv_out}/patched/klatt_synth.c"
  "${openevv_out}/patched/eci_api.c"
  "${openevv_out}/delta_langs.c"
  ${openevv_language_sources}
  ${openevv_rule_sources}
  src/synthesis/effects/openevv/atlas_openevv.c)
target_include_directories(atlas_openevv
  PRIVATE "${openevv}/include" ${openevv_source_dirs} ${openevv_language_includes}
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/synthesis/effects/openevv")
# What OpenEVV's Makefile compiles with: GNU C, no strict aliasing (the rules
# read one cell at several widths), the arena that keeps the machine's
# thirty-two bit references, and no warnings (it is transcribed code). The
# library is linked into a plugin, so nothing it defines is exported.
target_compile_definitions(atlas_openevv PRIVATE EVV_ARENA=1 ECI_STATIC=1)
target_compile_options(atlas_openevv PRIVATE
  -std=gnu99 -O2 -fno-strict-aliasing -w -fvisibility=hidden
  -Werror=implicit-function-declaration -Werror=int-conversion -Werror=incompatible-pointer-types)
set_target_properties(atlas_openevv PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
find_package(Threads REQUIRED)
target_link_libraries(atlas_openevv PRIVATE Threads::Threads m)

target_link_libraries(Atlas PRIVATE atlas_openevv)
target_compile_definitions(Atlas PUBLIC ATLAS_HAS_OPENEVV=1)
target_include_directories(Atlas PRIVATE "${openevv}/include")
message(STATUS "Speech oscillator with OpenEVV (${ATLAS_OPENEVV_LANGUAGES})")
