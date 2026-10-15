cmake_minimum_required(VERSION 3.19)

if(NOT EXISTS "${FIXTURE}" OR NOT DEFINED DIAGNOSTIC_RUNTIME)
	message(FATAL_ERROR "Synthetic Vulkan fixture and explicit edition are required")
endif()
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef suffix)
set(report "${CMAKE_CURRENT_BINARY_DIR}/gpu-command-capture-${suffix}.log")
if(EXISTS "${report}")
	message(FATAL_ERROR "Command capture report must be fresh")
endif()
foreach(probe IN ITEMS 0 1)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env --unset=XXII_GPU_OP_CHECKPOINT_START
			"XXII_COMPAT_PROBE=${probe}"
			"XXII_COMPAT_GPU_FLIGHT_RECORDER=1"
			"XXII_COMPAT_GPU_RESOURCE_LIFETIME=0"
			"XXII_COMPAT_GPU_FLIGHT_LOG=${report}"
			"${FIXTURE}" --htile-clear-only
		RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
		TIMEOUT 60)
	if(NOT status STREQUAL "0")
		file(REMOVE "${report}")
		message(FATAL_ERROR "Synthetic command fixture failed probe=${probe}: ${status}")
	endif()
	if(DIAGNOSTIC_RUNTIME AND "${probe}" STREQUAL "1")
		if(NOT EXISTS "${report}")
			message(FATAL_ERROR "Developer opt-in command capture did not persist")
		endif()
		file(READ "${report}" capture)
		file(REMOVE "${report}")
		if(NOT capture MATCHES "XXII_GPU_FLIGHT_BUFFER tick=[0-9]+ completed=[01] events=[1-9]" OR
				NOT capture MATCHES "XXII_GPU_FLIGHT_EVENT" OR
				NOT capture MATCHES "kind=image_barrier" OR
				capture MATCHES "XXII_GPU_RESOURCE_")
			message(FATAL_ERROR "Developer command capture lost real commands or activation isolation")
		endif()
	elseif(EXISTS "${report}" OR error MATCHES "XXII_GPU_(FLIGHT|BREADCRUMB|CHECKPOINT)")
		file(REMOVE "${report}")
		message(FATAL_ERROR "Command capture executed without diagnostic edition and opt-in")
	endif()
endforeach()
message(STATUS "GPU command capture PASS diagnostic=${DIAGNOSTIC_RUNTIME} real_fixture=ON controls=2")
