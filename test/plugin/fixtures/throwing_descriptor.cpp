/// Loader fixture: a plugin whose get_descriptor() THROWS across the C ABI.
///
/// The plugin C ABI forbids exceptions from every entry point. The host's
/// loader is the boundary that contains this one: it must report a contract
/// violation and let no exception escape — not even std::bad_alloc, and not a
/// second exception raised while the detailed diagnostic is formatted.
///
/// One source, three variants selected by a compile definition:
///   YADDNSC_FIXTURE_DESCRIPTOR_RUNTIME_ERROR → std::runtime_error
///   YADDNSC_FIXTURE_DESCRIPTOR_NON_STD       → an exception outside std::exception
///   YADDNSC_FIXTURE_DESCRIPTOR_BAD_ALLOC     → std::bad_alloc
///
/// The non-standard variant throws without allocating, so an injected
/// allocation fault can only be absorbed by the host's diagnostic formatting.

#include <new>
#include <stdexcept>

#include <yaddnsc/sdk/driver_abi.h>

#define FIXTURE_EXPORT __attribute__((visibility("default")))

extern "C" FIXTURE_EXPORT yaddnsc_status yaddnsc_driver_get_descriptor(const yaddnsc_driver_descriptor** /*out*/) {
#if defined(YADDNSC_FIXTURE_DESCRIPTOR_RUNTIME_ERROR)
    throw std::runtime_error("descriptor exploded");
#elif defined(YADDNSC_FIXTURE_DESCRIPTOR_NON_STD)
    // A non-std exception type — exercises the catch-all firewall arm.
    throw 42;
#elif defined(YADDNSC_FIXTURE_DESCRIPTOR_BAD_ALLOC)
    throw std::bad_alloc{};
#else
#error "throwing_descriptor fixture needs a descriptor-throw variant"
#endif
}
