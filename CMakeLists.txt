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

add_executable(Stitchup WIN32 src/app.cpp resources/app.rc)
target_include_directories(Stitchup PRIVATE src ${PDFIUM_DIR}/include)
target_link_libraries(Stitchup PRIVATE gdi32 user32 shell32 comdlg32 comctl32
                      uxtheme dwmapi
                      ${PDFIUM_DIR}/lib/pdfium.dll.lib)

add_custom_command(TARGET Stitchup POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${PDFIUM_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:Stitchup>"
  COMMENT "Copying pdfium.dll next to Stitchup.exe")