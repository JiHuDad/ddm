# symbol_audit.cmake — assert the tap hot-path object references NO forbidden
# symbols (malloc/new/free/throw/lock/syscall). Run via:
#   cmake -DOBJ=<tap_hot.o> -DNM=<nm path> -P symbol_audit.cmake
# Air-gapped: needs only binutils `nm`. Proves NFR1/NFR6 statically.

if(NOT EXISTS "${OBJ}")
  message(FATAL_ERROR "tap hot object not found: ${OBJ}")
endif()
if(NOT NM)
  set(NM nm)
endif()

execute_process(
  COMMAND ${NM} --undefined-only "${OBJ}"
  OUTPUT_VARIABLE undef
  RESULT_VARIABLE rc
  ERROR_VARIABLE  nm_err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nm failed on ${OBJ}: ${nm_err}")
endif()

# Forbidden undefined symbols. Mangled names: _Znwm/_Znam = operator new,
# _ZdlPv/_ZdaPv = operator delete.
set(forbidden
  "malloc" "calloc" "realloc" "free"
  "_Znwm" "_Znam" "_ZdlPv" "_ZdaPv"
  "__cxa_throw" "__cxa_allocate_exception" "__cxa_rethrow"
  "pthread_mutex_lock" "pthread_mutex_unlock"
  "mmap" "munmap" "open" "read" "write")

set(hits "")
foreach(sym ${forbidden})
  # Match the symbol as a whole token in an `U <sym>` line.
  if(undef MATCHES "U[ \t]+_?${sym}([ \t@]|$)" OR undef MATCHES "U[ \t]+_?${sym}\n")
    list(APPEND hits ${sym})
  endif()
endforeach()

if(hits)
  message("---- nm --undefined-only ${OBJ} ----")
  message("${undef}")
  message(FATAL_ERROR "tap hot path references forbidden symbols: ${hits}")
endif()
message(STATUS "tap_symbol_audit PASS: no forbidden symbols in ${OBJ}")
