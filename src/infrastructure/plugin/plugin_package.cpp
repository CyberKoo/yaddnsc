#include "plugin_package.h"

#include <memory>
#include <utility>

#include "infrastructure/plugin/shared_library.h"

namespace plugin {

PluginPackage::PluginPackage(std::shared_ptr<SharedLibrary> library, PluginDescriptor descriptor,
                             domain::DriverCapabilities capabilities)
    : library_(std::move(library)), descriptor_(std::move(descriptor)), capabilities_(std::move(capabilities)) {}

PluginPackage::~PluginPackage() = default;

const std::string& PluginPackage::path() const noexcept {
    return library_->path();
}

}  // namespace plugin
