# Spring supplied CMake build file

set(MINIZIP_FIND_QUIETLY TRUE)
FIND_PACKAGE_STATIC(MiniZip)

if     (MINIZIP_FOUND)
	# Use the system supplied MiniZip library
	message(STATUS "Using minizip library: ${MINIZIP_LIBRARY}")

	set_global(SPRING_MINIZIP_INCLUDE_DIR "${MINIZIP_INCLUDE_DIR}")
	set_global(SPRING_MINIZIP_LIBRARY "${MINIZIP_LIBRARY}")
else   (MINIZIP_FOUND)
	# Build our own minizip library
	
	set(miniZipSources
			"unzip.c"
			"zip.c"
			"ioapi.c"
		)

	add_definitions(-DNOCRYPT -DNOUNCRYPT)
	find_package(ZLIB 1.2.7 REQUIRED)

	add_library(minizip STATIC EXCLUDE_FROM_ALL ${miniZipSources})
	target_link_libraries(minizip ZLIB::ZLIB)
	set_target_properties(minizip PROPERTIES COMPILE_FLAGS "${PIC_FLAG}")

	set_global(SPRING_MINIZIP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/rts/lib")
	set_global(SPRING_MINIZIP_LIBRARY minizip)
endif  (MINIZIP_FOUND)
