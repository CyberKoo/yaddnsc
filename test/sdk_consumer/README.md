# Installed SDK examples

Build these sources without the host checkout:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build build
ctest --test-dir build --output-on-failure
```

The C11 example links `yaddnsc::plugin_abi`; C++ HTTP uses `plugin_sdk`;
signing uses `plugin_sdk_crypto` and `<yaddnsc/sdk/signing.hpp>`; XML
uses `plugin_sdk_xml` when available. Glaze must be exactly v7.8.2 (bundled
with the development package). The crypto example uses two translation units.
`-DYADDNSC_CONSUMER_C_ONLY=ON` configures with only a C compiler and no Glaze.
On libstdc++ toolchains, `-DYADDNSC_CONSUMER_OLD_CXX_ABI=ON` exercises the
alternate standard-library ABI without a precompiled signing archive.

Each module exports its C ABI entry points with default visibility; other
symbols are hidden. The output matches its descriptor name (for
example `consumer_http.so`); copy it into the directory configured as
`driver_dir` (normally `<prefix>/lib/yaddnsc/drivers`), and use that descriptor
name in your driver configuration. The loader program supplies fake services
and runs a create/update/validate/destroy cycle without contacting providers.

CMake is the supported consumption entry point. Headers under
`yaddnsc/sdk` and `yaddnsc/util` are supported source APIs; the C ABI alone
is a binary compatibility contract.
