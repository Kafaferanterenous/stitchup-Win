cmake_minimum_required(VERSION 3.20)
project(StitchupPDFEditor CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /WX /O2 /MT /utf-8 /DNOMINMAX
                      /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00
                      /D_CRT_SECURE_NO_WARNINGS)
endif()

set(PDFIUM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium")

# Single source of truth for the version: the VERSION file at the repo root.
# It feeds both the exe's title bar and the VERSIONINFO resource, so the tag
# and the binary can no longer drift apart.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" STITCHUP_VERSION_RAW
     LIMIT_COUNT 1)
string(STRIP "${STITCHUP_VERSION_RAW}" STITCHUP_VERSION)
if(NOT STITCHUP_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION must look like MAJOR.MINOR.PATCH, got '${STITCHUP_VERSION}'")
endif()
string(REPLACE "." ";" STITCHUP_VER_PARTS "${STITCHUP_VERSION}")
list(GET STITCHUP_VER_PARTS 0 STITCHUP_VER_MAJOR)
list(GET STITCHUP_VER_PARTS 1 STITCHUP_VER_MINOR)
list(GET STITCHUP_VER_PARTS 2 STITCHUP_VER_PATCH)
message(STATUS "Stitchup version ${STITCHUP_VERSION}")

# Generate the .rc into the build tree so the source tree has exactly one
# version-bearing file (VERSION) and nothing hand-editable to forget.
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico"
     STITCHUP_RC_ICON)
configure_file(resources/app.rc.in resources/app.rc @ONLY)
set(STITCHUP_RC "${CMAKE_CURRENT_BINARY_DIR}/resources/app.rc")

add_executable(Stitchup WIN32 src/app.cpp ${STITCHUP_RC})
target_compile_definitions(Stitchup PRIVATE
                           STITCHUP_VERSION=L"${STITCHUP_VERSION}")
target_include_directories(Stitchup PRIVATE src ${PDFIUM_DIR}/include)
target_link_libraries(Stitchup PRIVATE gdi32 user32 shell32 comdlg32 comctl32
                      uxtheme dwmapi version
                      ${PDFIUM_DIR}/lib/pdfium.dll.lib)

add_custom_command(TARGET Stitchup POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${PDFIUM_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:Stitchup>"
  COMMENT "Copying pdfium.dll next to Stitchup.exe")