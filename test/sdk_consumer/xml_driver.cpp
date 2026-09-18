#include <yaddnsc/sdk/driver.hpp>
#include <yaddnsc/sdk/xml_raii.hpp>

namespace sdk = yaddnsc::sdk;

class XmlDriver final : public yaddnsc::sdk::Driver {
public:
    [[nodiscard]] sdk::Result update(sdk::UpdateContext &context) override {
        (void) context;
        xml_raii::unique_doc doc(xmlReadMemory("<root/>", 7, nullptr, nullptr, XML_PARSE_NONET));
        if (!doc) {
            return std::unexpected(sdk::Error{YADDNSC_STATUS_INTERNAL_ERROR, "libxml2 parse failed", 0});
        }
        return {};
    }
};

YADDNSC_DEFINE_DRIVER(XmlDriver, "consumer_xml", "installed XML SDK consumer", "yaddnsc", "1",
                      YADDNSC_DRIVER_CAPABILITY_A)
