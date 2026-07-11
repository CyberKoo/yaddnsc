//
// ResolverDispatcher unit tests — legacy system backend (libresolv, default).
//
// Compiled with the legacy dispatcher_system.cpp.  See
// test/fixtures/dispatcher_tests.h for the shared test bodies.  The shared
// header branches assertions on YADDNSC_NATIVE_DISPATCHER to account for the
// legacy backend's empty-vector collapse on non-definitive failures.
// =============================================================================

#include "fixtures/dispatcher_tests.h"
