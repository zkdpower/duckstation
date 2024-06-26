set(SRCS
  include/glad/gl.h
  include/KHR/khrplatform.h
  src/gl.c
)

# Linking as a static library breaks on macOS, see https://github.com/libigl/libigl/issues/751
if(APPLE)
  add_library(glad OBJECT ${SRCS})
else()
  add_library(glad ${SRCS})
endif()

target_include_directories(glad PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(glad PRIVATE Threads::Threads "${CMAKE_DL_LIBS}")

if(WIN32)
  target_sources(glad PRIVATE
    include/glad/wgl.h
    src/wgl.c
  )
else()
  target_sources(glad PRIVATE
    include/EGL/eglplatform.h
    include/glad/egl.h
    src/egl.c
  )
endif()
