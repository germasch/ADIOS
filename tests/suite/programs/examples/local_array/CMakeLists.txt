include_directories(${PROJECT_SOURCE_DIR}/src/public)
include_directories(${PROJECT_SOURCE_DIR}/src)
include_directories(${PROJECT_SOURCE_DIR}/tests/suite/programs/examples/local_array)
include_directories(${PROJECT_BINARY_DIR}/src)
include_directories(${PROJECT_BINARY_DIR}/src/public)
include_directories(${PROJECT_BINARY_DIR}/tests/suite/programs/examples/local_array)
link_directories(${PROJECT_BINARY_DIR}/test/suite/programs/examples/local_array)

add_executable(local_array_write_C local_array_write_C.c gwrite_arrays.ch)
if(MPI_COMPILE_FLAGS)
  set_target_properties(local_array_write_C PROPERTIES COMPILE_FLAGS "${MPI_C_COMPILE_FLAGS}")
endif()
if(MPI_LINK_FLAGS)
  set_target_properties(local_array_write_C PROPERTIES LINK_FLAGS "${MPI_C_LINK_FLAGS}")
endif()
target_link_libraries(local_array_write_C adios ${ADIOSLIB_LDADD} ${MPI_C_LIBRARIES})
add_custom_command(
	OUTPUT gwrite_arrays.ch
	COMMAND ${PROJECT_BINARY_DIR}/utils/gpp/gpp.py ${PROJECT_SOURCE_DIR}/tests/suite/programs/examples/local_array/local_array_C.xml
	DEPENDS local_array_C.xml
	)


add_executable(local_array_read_C local_array_read_C.c)
if(MPI_COMPILE_FLAGS)
  set_target_properties(local_array_read_C PROPERTIES COMPILE_FLAGS "${MPI_C_COMPILE_FLAGS}")
endif()
if(MPI_LINK_FLAGS)
  set_target_properties(local_array_read_C PROPERTIES LINK_FLAGS "${MPI_C_LINK_FLAGS}")
endif()
target_link_libraries(local_array_read_C adiosread ${ADIOSREADLIB_LDADD} ${MPI_C_LIBRARIES})

file(COPY local_array_C.xml
     DESTINATION ${PROJECT_BINARY_DIR}/tests/suite/programs/examples/local_array)

