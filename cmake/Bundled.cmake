# SQLite and Mbed TLS are compiled into the runtime archive, so Flywheel programs need no
# system SQLite or OpenSSL. Both are downloaded once at configure time from pinned, hash-checked
# release archives. For an offline build, point FETCHCONTENT_SOURCE_DIR_SQLITE and
# FETCHCONTENT_SOURCE_DIR_MBEDTLS at extracted copies of the same releases.
include(FetchContent)

FetchContent_Declare(sqlite
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
  URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(sqlite)
target_sources(flywheel_runtime PRIVATE "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(flywheel_runtime SYSTEM PRIVATE "${sqlite_SOURCE_DIR}")
# Loadable extensions would reintroduce a dynamic-loading dependency; nothing uses them.
set_source_files_properties("${sqlite_SOURCE_DIR}/sqlite3.c" PROPERTIES
  COMPILE_DEFINITIONS SQLITE_OMIT_LOAD_EXTENSION)

# Only the merged runtime archive is installed, not Mbed TLS's own libraries and headers.
set(_fw_exclude "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
  set(_fw_exclude EXCLUDE_FROM_ALL)
endif()
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.1/mbedtls-4.1.1.tar.bz2
  URL_HASH SHA256=3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  ${_fw_exclude})
set(INSTALL_MBEDTLS_HEADERS OFF CACHE BOOL "" FORCE)
set(INSTALL_TF_PSA_CRYPTO_HEADERS OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_SHARED_TF_PSA_CRYPTO_LIBRARY OFF CACHE BOOL "" FORCE)
# Thread safety (pthreads, or the runtime's Windows primitives through threading_alt.h).
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${PROJECT_SOURCE_DIR}/runtime/bundled/tls_config.h"
  CACHE FILEPATH "" FORCE)
# The objects are linked into position-independent programs and the compiler's JIT.
set(_fw_saved_pic ${CMAKE_POSITION_INDEPENDENT_CODE})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include_directories("${PROJECT_SOURCE_DIR}/runtime/bundled")
if(NOT MSVC)
  add_compile_options(-ffunction-sections -fdata-sections)
endif()
FetchContent_MakeAvailable(mbedtls)
set(CMAKE_POSITION_INDEPENDENT_CODE ${_fw_saved_pic})

set(_fw_tls_targets mbedtls mbedx509 tfpsacrypto)
add_dependencies(flywheel_runtime ${_fw_tls_targets})
foreach(_fw_tls IN LISTS _fw_tls_targets)
  target_include_directories(flywheel_runtime SYSTEM PRIVATE
    $<TARGET_PROPERTY:${_fw_tls},INTERFACE_INCLUDE_DIRECTORIES>)
  target_compile_definitions(flywheel_runtime PRIVATE
    $<TARGET_PROPERTY:${_fw_tls},INTERFACE_COMPILE_DEFINITIONS>)
endforeach()
if(WIN32)
  # BCryptGenRandom (entropy) and the system root certificate store.
  target_link_libraries(flywheel_runtime PUBLIC bcrypt crypt32)
endif()
if(NOT MSVC)
  target_compile_options(flywheel_runtime PRIVATE -ffunction-sections -fdata-sections)
endif()

# Merge the Mbed TLS archives into the runtime archive after it is built, so programs and
# installs still link exactly one archive.
add_custom_command(TARGET flywheel_runtime POST_BUILD
  COMMAND ${CMAKE_COMMAND}
    -DAR=${CMAKE_AR} -DMSVC=${MSVC} -DAPPLE=${APPLE}
    -DOUTPUT=$<TARGET_FILE:flywheel_runtime>
    "-DINPUTS=$<TARGET_FILE:mbedtls>|$<TARGET_FILE:mbedx509>|$<TARGET_FILE:tfpsacrypto>"
    -P ${PROJECT_SOURCE_DIR}/cmake/MergeArchives.cmake
  VERBATIM)
