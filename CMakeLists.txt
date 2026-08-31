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
  target_sources(portfolio PRIVATE src/renderer.cpp)

  set(SITE_URL "https://phong.computer/")
  set(COPYRIGHT_YEAR "2026")
  set(SITE_DIR "${CMAKE_BINARY_DIR}/site")

  execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE ASSET_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
  )
  if(NOT ASSET_VERSION)
    set(ASSET_VERSION dev)
  endif()

  set(BUNDLE_NAME "portfolio.${ASSET_VERSION}")
  set(STYLE_NAME "styles.${ASSET_VERSION}.css")
  set(FAVICON_SVG_NAME "favicon.${ASSET_VERSION}.svg")
  set(FAVICON_PNG_NAME "favicon.${ASSET_VERSION}.png")

  set_target_properties(
    portfolio
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SITE_DIR}"
               OUTPUT_NAME "${BUNDLE_NAME}"
               SUFFIX ".js"
  )

  target_compile_options(portfolio PRIVATE -Oz -flto)
  target_link_options(
    portfolio
    PRIVATE
    -Oz
    -flto
    "SHELL:--closure 1"
    "SHELL:-sINCOMING_MODULE_JS_API=[]"
    "SHELL:-sENVIRONMENT=web"
    "SHELL:-sFILESYSTEM=0"
    "SHELL:-sMALLOC=emmalloc"
    "SHELL:-sEXIT_RUNTIME=0"
    "SHELL:-sALLOW_MEMORY_GROWTH=0"
    "SHELL:-sMIN_WEBGL_VERSION=2"
    "SHELL:-sMAX_WEBGL_VERSION=2"
  )

  file(GLOB STALE_ASSETS "${SITE_DIR}/portfolio*" "${SITE_DIR}/styles*" "${SITE_DIR}/favicon*")
  list(
    REMOVE_ITEM
    STALE_ASSETS
    "${SITE_DIR}/${BUNDLE_NAME}.js"
    "${SITE_DIR}/${BUNDLE_NAME}.wasm"
    "${SITE_DIR}/${STYLE_NAME}"
    "${SITE_DIR}/${FAVICON_SVG_NAME}"
    "${SITE_DIR}/${FAVICON_PNG_NAME}"
  )
  if(STALE_ASSETS)
    file(REMOVE ${STALE_ASSETS})
  endif()

  configure_file("${PROJECT_SOURCE_DIR}/web/index.html" "${SITE_DIR}/index.html" @ONLY)
  configure_file("${PROJECT_SOURCE_DIR}/web/styles.css" "${SITE_DIR}/${STYLE_NAME}" COPYONLY)
  configure_file("${PROJECT_SOURCE_DIR}/web/favicon.svg" "${SITE_DIR}/${FAVICON_SVG_NAME}" COPYONLY)
  configure_file("${PROJECT_SOURCE_DIR}/web/favicon.png" "${SITE_DIR}/${FAVICON_PNG_NAME}" COPYONLY)
  configure_file("${PROJECT_SOURCE_DIR}/web/_headers" "${SITE_DIR}/_headers" @ONLY)
endif()
