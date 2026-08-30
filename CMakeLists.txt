cmake_minimum_required(VERSION 3.25)

project(
  portfolio
  VERSION 0.1.0
  LANGUAGES CXX
)

add_executable(portfolio src/main.cpp)

target_compile_features(portfolio PRIVATE cxx_std_23)
set_target_properties(portfolio PROPERTIES CXX_EXTENSIONS OFF)

target_compile_options(
  portfolio
  PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

if(EMSCRIPTEN)
  set(SITE_DIR "${CMAKE_BINARY_DIR}/site")

  set_target_properties(portfolio PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SITE_DIR}" SUFFIX ".js")

  target_link_options(
    portfolio PRIVATE "SHELL:-sENVIRONMENT=web" "SHELL:-sFILESYSTEM=0"
    "SHELL:-sMIN_WEBGL_VERSION=2" "SHELL:-sMAX_WEBGL_VERSION=2"
  )

  set(WEB_ASSETS index.html styles.css)
  foreach(web_asset IN LISTS WEB_ASSETS)
    configure_file("${PROJECT_SOURCE_DIR}/web/${web_asset}" "${SITE_DIR}/${web_asset}" COPYONLY)
  endforeach()
endif()
