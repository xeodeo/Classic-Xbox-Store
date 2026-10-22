# Classic Xbox Store: tienda de juegos para la Xbox original.
#
# Mismo sistema de compilacion que el port de OptiCraft:
#   1. cl.exe de VS2022 compila C++17 contra la STL + UCRT modernas.
#   2. El Link.Exe del XDK 5849 enlaza con el CRT estatico de VS2022 y las
#      bibliotecas del XDK (XAPI, D3D8, XNet).
#   3. src/runtime/XboxCrtShim.cpp pone las funciones Win32 que el CRT moderno
#      necesita y XAPI no tiene; XboxEntry.cpp replica el arranque del XDK.
#   4. scripts/patch_pe_for_imagebld.ps1 y patch_sse2_moves.ps1 dejan el PE
#      listo para imagebld y sin SSE2 (la Xbox es un Pentium III).
#   5. imagebld crea default.xbe; extract-xiso empaqueta una ISO para xemu.
#
# Salida: bin/iso/default.xbe y bin/ClassicXboxStore.iso

cmake_minimum_required(VERSION 3.21)
project(ClassicXboxStore C CXX)
enable_language(ASM_MASM)

set(XBOX_TITLE_NAME "Classic Xbox Store" CACHE STRING "Nombre del titulo dentro del XBE")
set(XBOX_TITLE_ID "0xFFFF4358" CACHE STRING "ID de titulo de prueba ('CX')")
set(XBOX_STACK_SIZE "0x40000" CACHE STRING "Pila del hilo principal")
option(STORE_AUTOTEST "Prueba para xemu: instala solo el primer juego de la A (nunca se publica)" OFF)
if(STORE_AUTOTEST)
    set(XBOX_BIN_DIR "${CMAKE_SOURCE_DIR}/bin-autotest")
else()
    set(XBOX_BIN_DIR "${CMAKE_SOURCE_DIR}/bin")
endif()
set(XBOX_ISO_DIR "${XBOX_BIN_DIR}/iso")

# --- Arranque y CRT (siempre) -------------------------------------------------
set(RUNTIME_DIR "${CMAKE_SOURCE_DIR}/src/runtime")
set(XBOX_IMPORTS_ASM "${CMAKE_BINARY_DIR}/XboxImports.asm")
add_custom_command(
    OUTPUT "${XBOX_IMPORTS_ASM}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/gen_imports.ps1"
            -List "${RUNTIME_DIR}/XboxImports.txt"
            -Out "${XBOX_IMPORTS_ASM}"
    DEPENDS "${RUNTIME_DIR}/XboxImports.txt" "${CMAKE_SOURCE_DIR}/scripts/gen_imports.ps1"
    COMMENT "Generando los saltos Win32 del CRT"
    VERBATIM
)
set(RUNTIME_SOURCES
    "${RUNTIME_DIR}/XboxEntry.cpp"
    "${RUNTIME_DIR}/XboxCrtShim.cpp"
    "${RUNTIME_DIR}/XboxP3Math.c"
    "${XBOX_IMPORTS_ASM}"
)
set_property(SOURCE "${RUNTIME_DIR}/XboxP3Math.c" APPEND PROPERTY COMPILE_OPTIONS "/arch:IA32")

# --- Codigo de la tienda ------------------------------------------------------
file(GLOB_RECURSE STORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.c")
list(FILTER STORE_SOURCES EXCLUDE REGEX "[/\\]src[/\\]runtime[/\\]")

# Fuentes de letra (Open Sans, licencia Apache 2.0) metidas dentro del XBE.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(FONT_DIR "${CMAKE_SOURCE_DIR}/assets/fonts")
set(EMBEDDED_CPP "${CMAKE_BINARY_DIR}/EmbeddedFiles.cpp")
add_custom_command(
    OUTPUT "${EMBEDDED_CPP}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/embed_files.py" "${EMBEDDED_CPP}"
            "kFontRegular=${FONT_DIR}/OpenSans-Regular.ttf"
            "kFontSemibold=${FONT_DIR}/OpenSans-Semibold.ttf"
            "kRootCerts=${CMAKE_SOURCE_DIR}/assets/certs/roots.pem"
            "kArtIndex=${CMAKE_SOURCE_DIR}/assets/data/artindex.tsv"
    DEPENDS "${CMAKE_SOURCE_DIR}/scripts/embed_files.py"
            "${FONT_DIR}/OpenSans-Regular.ttf" "${FONT_DIR}/OpenSans-Semibold.ttf"
            "${CMAKE_SOURCE_DIR}/assets/certs/roots.pem"
            "${CMAKE_SOURCE_DIR}/assets/data/artindex.tsv"
    COMMENT "Metiendo fuentes y certificados en el XBE"
    VERBATIM
)

# mbedTLS (Apache 2.0) para HTTPS, con nuestra configuracion minima.
file(GLOB MBEDTLS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/external/mbedtls/library/*.c")

# zlib (solo descomprimir): la Xbox descomprime ella misma el ZIP de archive.org.
set(ZLIB_DIR "${CMAKE_SOURCE_DIR}/external/zlib")
set(ZLIB_SOURCES
    "${ZLIB_DIR}/adler32.c" "${ZLIB_DIR}/crc32.c" "${ZLIB_DIR}/inffast.c"
    "${ZLIB_DIR}/inflate.c" "${ZLIB_DIR}/inftrees.c" "${ZLIB_DIR}/zutil.c")
configure_file("${ZLIB_DIR}/zconf.h.cmakein" "${CMAKE_BINARY_DIR}/zlib/zconf.h" @ONLY)

add_executable(ClassicXboxStore ${STORE_SOURCES} ${RUNTIME_SOURCES} "${EMBEDDED_CPP}" ${MBEDTLS_SOURCES}
               ${ZLIB_SOURCES})
set_target_properties(ClassicXboxStore PROPERTIES
    SUFFIX ".exe"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${XBOX_BIN_DIR}"
)

target_compile_options(ClassicXboxStore PRIVATE
    $<$<COMPILE_LANGUAGE:C,CXX>:/arch:SSE>        # Pentium III: SSE, sin SSE2
    $<$<COMPILE_LANGUAGE:C,CXX>:/O2>
    $<$<COMPILE_LANGUAGE:C,CXX>:/GS->
    $<$<COMPILE_LANGUAGE:C,CXX>:/Gy>
    $<$<COMPILE_LANGUAGE:C,CXX>:/Gw>
    $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    $<$<COMPILE_LANGUAGE:CXX>:/GR>
    $<$<COMPILE_LANGUAGE:CXX>:/d2FH4->
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
    $<$<COMPILE_LANGUAGE:C,CXX>:/wd4996>
)
target_compile_definitions(ClassicXboxStore PRIVATE
    "_XBOX"
    "XBOX_PLATFORM"
    "NDEBUG"
    "NOMINMAX"
    "_CRT_SECURE_NO_WARNINGS"
    "_M_FP_FAST"
    "_USE_STD_VECTOR_ALGORITHMS=0"
    "MBEDTLS_CONFIG_FILE=\"net/mbedtls_config.h\""
    STORE_AUTOTEST=$<BOOL:${STORE_AUTOTEST}>
)
target_include_directories(ClassicXboxStore PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/external"
    "${CMAKE_SOURCE_DIR}/external/mbedtls/include"
    "${CMAKE_BINARY_DIR}/zlib"
    "${CMAKE_SOURCE_DIR}/external/zlib"
    ${XBOX_STL_INCLUDES}
)
# Las cabeceras del XDK van DESPUES de la STL moderna (el XDK trae su STL de
# 2003, que no debe ganar). El arranque (src/runtime) no las usa.
set_source_files_properties(${STORE_SOURCES} PROPERTIES
    COMPILE_OPTIONS "/I${XBOX_XDK_ROOT}/include")
# mbedTLS incluye <windows.h> en _WIN32 (solo para SecureZeroMemory, que es en
# linea): le damos las cabeceras Win32, nunca las del XDK.
set(_win32_include_flags "")
foreach(_dir IN LISTS XBOX_WIN32_INCLUDES)
    list(APPEND _win32_include_flags "/I${_dir}")
endforeach()
set_source_files_properties(${MBEDTLS_SOURCES} ${ZLIB_SOURCES} PROPERTIES COMPILE_OPTIONS "${_win32_include_flags}")

# --- Enlace -------------------------------------------------------------------
set(XBOX_LINK_LIBS
    "${XBOX_MSVC_LIB_DIR}/libcmt.lib"
    "${XBOX_MSVC_LIB_DIR}/libcpmt.lib"
    "${XBOX_MSVC_LIB_DIR}/libvcruntime.lib"
    "${XBOX_UCRT_LIB_DIR}/libucrt.lib"
    "${XBOX_MSVC_LIB_DIR}/legacy_stdio_definitions.lib"
    "${XBOX_MSVC_LIB_DIR}/legacy_stdio_wide_specifiers.lib"
    "${XBOX_MSVC_LIB_DIR}/oldnames.lib"
    "${XBOX_XDK_ROOT}/lib/xapilib.lib"
    "${XBOX_XDK_ROOT}/lib/d3d8.lib"
    "${XBOX_XDK_ROOT}/lib/d3dx8.lib"
    "${XBOX_XDK_ROOT}/lib/xgraphics.lib"
    # XNet de desarrollo: la unica que puede hablar con servidores normales.
    "${XBOX_XDK_ROOT}/lib/xnet.lib"
    "${XBOX_XDK_ROOT}/lib/xboxkrnl.lib"
)
string(REPLACE ";" "\" \"" _xbox_libs_quoted "${XBOX_LINK_LIBS}")
set(CMAKE_CXX_LINK_EXECUTABLE
    "\"${CMAKE_LINKER}\" /nologo /MACHINE:I386 /FIXED:NO /SUBSYSTEM:WINDOWS /NODEFAULTLIB /ENTRY:XboxEntry /INCREMENTAL:NO /OPT:REF /OPT:ICF /MAP:<TARGET>.map /OUT:<TARGET> <OBJECTS> \"${_xbox_libs_quoted}\"")

# --- Despues de enlazar: parches, XBE, ISO ------------------------------------
get_filename_component(_xbox_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(XBOX_XBE "${XBOX_ISO_DIR}/default.xbe")

# Icono (assets/icon, lo genera scripts/make_icon.py): el bundler del XDK lo
# pasa a XPR e imagebld lo mete como imagen del titulo y de las partidas.
set(ICON_DIR "${CMAKE_SOURCE_DIR}/assets/icon")
set(XBOX_TITLE_IMAGE "${XBOX_BIN_DIR}/titleimage.xpr")
set(XBOX_SAVE_IMAGE "${XBOX_BIN_DIR}/saveimage.xpr")
add_custom_command(TARGET ClassicXboxStore PRE_LINK
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XBOX_BIN_DIR}"
    COMMAND "${XBOX_XDK_ROOT}/bin/bundler.exe" titleimage.rdf -o "${XBOX_TITLE_IMAGE}" -q
    COMMAND "${XBOX_XDK_ROOT}/bin/bundler.exe" saveimage.rdf -o "${XBOX_SAVE_IMAGE}" -q
    WORKING_DIRECTORY "${ICON_DIR}"
    COMMENT "bundler: imagen del titulo y de las partidas"
    VERBATIM)
add_custom_command(TARGET ClassicXboxStore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XBOX_ISO_DIR}"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/patch_pe_for_imagebld.ps1"
            -Path "$<TARGET_FILE:ClassicXboxStore>"
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/scripts/patch_sse2_moves.ps1"
            -Path "$<TARGET_FILE:ClassicXboxStore>" -Map "$<TARGET_FILE:ClassicXboxStore>.map"
            -Dumpbin "${_xbox_cl_dir}/dumpbin.exe"
    COMMAND "${XBOX_XDK_ROOT}/bin/imagebld.exe"
            /NOLOGO /STACK:${XBOX_STACK_SIZE} /TESTID:${XBOX_TITLE_ID} /TESTREGION:0x80000007
            /TESTMEDIATYPES:0x400003FF "/TESTNAME:${XBOX_TITLE_NAME}" /NOLIBWARN /LIMITMEM
            "/TITLEIMAGE:${XBOX_TITLE_IMAGE}" "/DEFAULTSAVEIMAGE:${XBOX_SAVE_IMAGE}"
            "/IN:$<TARGET_FILE:ClassicXboxStore>" "/MAP:$<TARGET_FILE:ClassicXboxStore>.map" "/OUT:${XBOX_XBE}"
    # Miniatura para paneles tipo XBMC / UnleashX.
    COMMAND ${CMAKE_COMMAND} -E copy "${ICON_DIR}/cover.png" "${XBOX_ISO_DIR}/default.tbn"
    COMMENT "imagebld: ${XBOX_XBE}"
    VERBATIM
)
file(TO_NATIVE_PATH "${XBOX_ISO_DIR}" _iso_dir_native)
file(TO_NATIVE_PATH "${XBOX_BIN_DIR}/ClassicXboxStore.iso" _iso_native)
add_custom_command(TARGET ClassicXboxStore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f "${XBOX_BIN_DIR}/ClassicXboxStore.iso"
    COMMAND "${CMAKE_SOURCE_DIR}/scripts/extract-xiso.exe" -c "${_iso_dir_native}" "${_iso_native}"
    COMMENT "extract-xiso: bin/ClassicXboxStore.iso"
    VERBATIM
)
