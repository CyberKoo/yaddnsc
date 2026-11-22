#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_PACKAGE_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_PACKAGE_H

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "domain/driver/capabilities.h"

namespace domain {
struct DriverUpdateCommand;
}

class SharedLibrary;

namespace plugin {
class DriverSession;
struct SessionDependencies;

/// Host-owned, domain-facing metadata for one validated driver package.
struct PluginDescriptor {
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::uint16_t abi_major{0};
    std::uint16_t abi_minor{0};
};

/// An immutable, validated driver image. Catalog and gateway consumers see no
/// raw ABI capability bits or loading state. Concrete packages create their
/// protocol sessions and keep the image alive through every session drain.
class PluginPackage : public std::enable_shared_from_this<PluginPackage> {
public:
    virtual ~PluginPackage();

    PluginPackage(const PluginPackage&) = delete;
    PluginPackage& operator=(const PluginPackage&) = delete;

    [[nodiscard]] const PluginDescriptor& descriptor() const noexcept { return descriptor_; }

    [[nodiscard]] const domain::DriverCapabilities& capabilities() const noexcept { return capabilities_; }

    [[nodiscard]] const std::string& path() const noexcept;

    [[nodiscard]] virtual bool supports_validate() const noexcept = 0;
    [[nodiscard]] virtual std::string_view validation_entry_name() const noexcept = 0;

    [[nodiscard]] virtual std::unique_ptr<DriverSession> create_update_session(
        domain::DriverUpdateCommand command, SessionDependencies dependencies) const = 0;
    [[nodiscard]] virtual std::unique_ptr<DriverSession> create_validation_session(
        std::string driver_param_json, SessionDependencies dependencies) const = 0;

protected:
    PluginPackage(std::shared_ptr<SharedLibrary> library, PluginDescriptor descriptor,
                  domain::DriverCapabilities capabilities);

private:
    std::shared_ptr<SharedLibrary> library_;
    PluginDescriptor descriptor_;
    domain::DriverCapabilities capabilities_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_PLUGIN_PACKAGE_H
