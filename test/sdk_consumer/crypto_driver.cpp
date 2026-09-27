#include <signing.h>
#include <yaddnsc/sdk/driver.hpp>

namespace sdk = yaddnsc::sdk;

class CryptoDriver final : public yaddnsc::sdk::Driver {
public:
    [[nodiscard]] sdk::Result update([[maybe_unused]] sdk::UpdateContext& context) override {
        if (Signing::sha256_hex("consumer").empty()) {
            return std::unexpected(sdk::Error{YADDNSC_STATUS_INTERNAL_ERROR, "OpenSSL SHA-256 failed", 0});
        }
        return {};
    }
};

YADDNSC_DEFINE_DRIVER(CryptoDriver, "consumer_crypto", "installed crypto SDK consumer", "yaddnsc", "1",
                      YADDNSC_DRIVER_CAPABILITY_A)
