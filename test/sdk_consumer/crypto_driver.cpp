#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/signing.hpp>

std::string consumer_hash();

namespace sdk = yaddnsc::sdk;

class CryptoDriver final : public yaddnsc::sdk::Driver {
public:
    yaddnsc::sdk::Result validate(std::string_view) const override { return {}; }

    [[nodiscard]] sdk::Result update([[maybe_unused]] sdk::UpdateContext& context) override {
        if (consumer_hash().empty() || consumer_hash() != sdk::signing::sha256_hex("consumer")) {
            return std::unexpected(sdk::Error{YADDNSC_STATUS_INTERNAL_ERROR, "OpenSSL SHA-256 failed", 0});
        }
        return {};
    }
};

YADDNSC_DEFINE_DRIVER(CryptoDriver, "consumer_crypto", "installed crypto SDK consumer", "yaddnsc", "1",
                      YADDNSC_DRIVER_CAPABILITY_A, 0)
