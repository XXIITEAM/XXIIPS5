#ifndef XXII_COMMON_DIAGNOSTICS_RUNTIME_TELEMETRY_H_
#define XXII_COMMON_DIAGNOSTICS_RUNTIME_TELEMETRY_H_

// Preserve standalone Kyty diagnostics; the PSVXXII Release runtime overrides
// this in CMake, independently of the compiler's Debug/Release configuration.
#ifndef KYTY_DIAGNOSTIC_RUNTIME
#define KYTY_DIAGNOSTIC_RUNTIME 1
#endif

#endif
