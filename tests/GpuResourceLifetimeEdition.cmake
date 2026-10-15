cmake_minimum_required(VERSION 3.19)

if(NOT EXISTS "${RUNTIME}" OR NOT DEFINED DIAGNOSTIC_RUNTIME)
	message(FATAL_ERROR "Runtime and explicit diagnostic edition are required")
endif()

# Real runtime entrypoint, no guest, graphics initialization or shared user state.
foreach(request IN ITEMS unset 0 1 10)
	if("${request}" STREQUAL "unset")
		set(control "--unset=XXII_COMPAT_GPU_RESOURCE_LIFETIME")
	else()
		set(control "XXII_COMPAT_GPU_RESOURCE_LIFETIME=${request}")
	endif()
	foreach(probe IN ITEMS 0 1)
		execute_process(
			COMMAND "${CMAKE_COMMAND}" -E env
				"${control}"
				"XXII_COMPAT_PROBE=${probe}"
				"${RUNTIME}" --help
			RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
			TIMEOUT 15)
		if(NOT DIAGNOSTIC_RUNTIME AND "${request}" STREQUAL "1")
			if(NOT status STREQUAL "2" OR NOT error STREQUAL
					"XXII_RUNTIME_OPTION_UNAVAILABLE edition=RELEASE feature=gpu_resource_lifetime required=DEVELOPER\n"
					OR NOT output STREQUAL "")
				message(FATAL_ERROR "Release must explicitly reject lifetime capture before startup")
			endif()
		elseif(NOT status STREQUAL "0" OR NOT error STREQUAL "" OR
				NOT output MATCHES "kyty_emulator --game")
			message(FATAL_ERROR "Unrequested or supported diagnostic capture must preserve normal help")
		endif()
	endforeach()
endforeach()

file(STRINGS "${RUNTIME}" markers REGEX "XXII_GPU_RESOURCE_(SUMMARY|EVENT)")
if(DIAGNOSTIC_RUNTIME)
	if(NOT markers MATCHES "XXII_GPU_RESOURCE_SUMMARY" OR NOT markers MATCHES "XXII_GPU_RESOURCE_EVENT")
		message(FATAL_ERROR "Developer lifetime capture output is missing")
	endif()
elseif(markers)
	message(FATAL_ERROR "Release retains diagnostic lifetime capture output")
endif()
file(STRINGS "${RUNTIME}" required REGEX "XXII_PRESENT_FPS frames=|XXII_GPU_SEMAPHORE_FAILURE")
if(NOT required MATCHES "XXII_PRESENT_FPS frames=" OR NOT required MATCHES "XXII_GPU_SEMAPHORE_FAILURE")
	message(FATAL_ERROR "Player progress or minimal GPU failure reporting was removed")
endif()
message(STATUS "GPU lifetime edition contract PASS diagnostic=${DIAGNOSTIC_RUNTIME} controls=8")
