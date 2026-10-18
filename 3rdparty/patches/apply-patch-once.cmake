# Idempotent FetchContent patch step. A reused source tree may already carry
# the patch (for example after the patch command line changed), and
# re-applying it would fail: skip when the patch reverses cleanly.
execute_process(
	COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${PATCH_FILE}"
	RESULT_VARIABLE patch_already_applied
	OUTPUT_QUIET ERROR_QUIET)
if(patch_already_applied EQUAL 0)
	message(STATUS "Patch already applied: ${PATCH_FILE}")
else()
	execute_process(
		COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${PATCH_FILE}"
		RESULT_VARIABLE patch_result)
	if(NOT patch_result EQUAL 0)
		message(FATAL_ERROR "Patch does not apply: ${PATCH_FILE}")
	endif()
endif()
