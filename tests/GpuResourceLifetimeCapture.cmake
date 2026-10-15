cmake_minimum_required(VERSION 3.19)

if(NOT EXISTS "${FIXTURE}")
	message(FATAL_ERROR "Synthetic Vulkan fixture is required")
endif()
string(RANDOM LENGTH 24 ALPHABET 0123456789abcdef suffix)
set(report "${CMAKE_CURRENT_BINARY_DIR}/gpu-resource-capture-${suffix}.log")
if(EXISTS "${report}")
	message(FATAL_ERROR "Capture report must be fresh")
endif()

foreach(probe IN ITEMS 0 1)
	execute_process(
		COMMAND "${CMAKE_COMMAND}" -E env
			"XXII_COMPAT_PROBE=${probe}"
			"XXII_COMPAT_GPU_FLIGHT_RECORDER=1"
			"XXII_COMPAT_GPU_RESOURCE_LIFETIME=1"
			"XXII_COMPAT_GPU_FLIGHT_LOG=${report}"
			"${FIXTURE}" --htile-clear-only
		RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error
		TIMEOUT 60)
	if(NOT status STREQUAL "0")
		file(REMOVE "${report}")
		message(FATAL_ERROR "Synthetic resource capture fixture failed probe=${probe}: ${status}")
	endif()
	if("${probe}" STREQUAL "0")
		if(EXISTS "${report}" OR error MATCHES "XXII_GPU_RESOURCE_")
			file(REMOVE "${report}")
			message(FATAL_ERROR "Resource capture activated without compatibility opt-in")
		endif()
	else()
		if(NOT EXISTS "${report}")
			message(FATAL_ERROR "Developer opt-in did not persist resource history")
		endif()
		file(READ "${report}" capture)
		file(REMOVE "${report}")
		if(NOT capture MATCHES "XXII_GPU_RESOURCE_SUMMARY events=[1-9][0-9]*" OR
				NOT capture MATCHES "kind=created" OR
				NOT capture MATCHES "kind=retire_queued" OR
				NOT capture MATCHES "kind=retire_executed" OR
				capture MATCHES "premature_retire=[1-9]")
			message(FATAL_ERROR "Developer capture lost real create/deferred-retirement hooks")
		endif()
	endif()
endforeach()
message(STATUS "GPU lifetime Developer capture PASS opt_in=required lifecycle=create,queue,execute")
