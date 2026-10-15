# Generates the NVDA controller RPC client stub with MIDL.
# cmake -DMIDL=... -DCL_DIR=... -DMIDL_ENV=x64|arm64|win32 -DSOURCE_DIR=... -DOUTPUT_DIR=... -P generate_stub.cmake
#
# MIDL runs cl.exe to preprocess the IDL, so the compiler's folder goes first
# on PATH. The flags match NVDA's own client build (nvdaHelper/client/sconscript).
set(ENV{PATH} "${CL_DIR};$ENV{PATH}")
execute_process(
  COMMAND "${MIDL}" /nologo /env ${MIDL_ENV} /server none /prefix all nvdaController_
    /header "${OUTPUT_DIR}/nvdaController.h" /cstub "${OUTPUT_DIR}/nvdaController_c.c"
    /acf "${SOURCE_DIR}/nvdaController.acf" "${SOURCE_DIR}/nvdaController.idl"
  WORKING_DIRECTORY "${OUTPUT_DIR}"
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "MIDL could not generate the NVDA controller stub (exit ${result})")
endif()
