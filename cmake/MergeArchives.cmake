# cmake -DAR=... -DOUTPUT=archive "-DINPUTS=a;b;c" [-DMSVC=1] [-DAPPLE=1] -P MergeArchives.cmake
# Appends the members of each INPUTS archive to OUTPUT, in place.
# INPUTS is a CMake list. Don't separate it with "|": cmd.exe treats that as a pipe on Windows.
set(merged "${OUTPUT}.merged")
file(REMOVE "${merged}")
if(MSVC)
  execute_process(COMMAND "${AR}" /NOLOGO "/OUT:${merged}" "${OUTPUT}" ${INPUTS}
                  RESULT_VARIABLE result)
elseif(APPLE)
  execute_process(COMMAND libtool -static -o "${merged}" "${OUTPUT}" ${INPUTS}
                  RESULT_VARIABLE result)
else()
  # GNU ar and llvm-ar accept an MRI script on standard input.
  set(script "CREATE ${merged}\nADDLIB ${OUTPUT}\n")
  foreach(input IN LISTS INPUTS)
    string(APPEND script "ADDLIB ${input}\n")
  endforeach()
  string(APPEND script "SAVE\nEND\n")
  file(WRITE "${OUTPUT}.mri" "${script}")
  execute_process(COMMAND "${AR}" -M INPUT_FILE "${OUTPUT}.mri" RESULT_VARIABLE result)
  file(REMOVE "${OUTPUT}.mri")
endif()
if(NOT result EQUAL 0)
  message(FATAL_ERROR "merging ${INPUTS} into ${OUTPUT} failed (${result})")
endif()
file(RENAME "${merged}" "${OUTPUT}")
