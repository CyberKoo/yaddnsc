//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <memory>
#include <utility>

#include <spdlog/spdlog.h>

#include "dns/resolver_factory.h"
#include "dns/multi_resolver.h"
#include "updater.h"
#include "driver_loader.h"
#include "scheduler.h"
#include "signal_handler.h"
#include "driver_manager.h"
#include "network/network_manager.h"
#include "config/config_validator.hpp"
#include "min_update_interval.h"

// ---------------------------------------------------------------------------
// Manager::Impl — orchestrates the lifecycle of all subsystem components.
//
// Responsibilities:
//   • Construct and wire up DriverManager, NetworkManager, MultiResolver,
//     Updater, Scheduler, and SignalHandler.
//   • Delegate driver loading to DriverLoader.
//   • Delegate config validation to ConfigValidator.
//   • Delegate signal-handler installation to SignalHandler (bridge its
//     stop_source into the Scheduler).
//   • Delegate the main event loop to Scheduler::run().
// ---------------------------------------------------------------------------
class Manager::Impl {
public:
    explicit Impl(Config::config config)
        : config_(std::move(config))
          , resolver_pool_(DnsResolverFactory::create(config_))
          , updater_(driver_manager_, network_manager_, resolver_pool_)
          , scheduler_(config_, updater_) {
    }

    void load_drivers() {
        DriverLoader::load(driver_manager_, config_);
    }

    void validate_config() const {
        const ConfigValidator<YADDNSC_MIN_UPDATE_INTERVAL> validator(driver_manager_, network_manager_);
        validator.validate(config_);
    }

    void install_signal_handler() {
        signal_handler_.install();
        scheduler_.set_stop_source(signal_handler_.get_stop_source());
    }

    void run() {
        const auto interfaces = network_manager_.get_interfaces();
        SPDLOG_INFO("All available interfaces: {}", fmt::join(interfaces, ", "));

        scheduler_.run();
    }

private:
    // IMPORTANT: destruction order is the reverse of declaration order.
    // config_ is declared first because it's needed by resolver_pool_'s constructor.
    Config::config config_;
    DriverManager driver_manager_;
    NetworkManager network_manager_;
    MultiResolver resolver_pool_;
    Updater updater_;
    Scheduler scheduler_;
    SignalHandler signal_handler_;
};

// ---------------------------------------------------------------------------
// Manager public API
// ---------------------------------------------------------------------------

Manager::Manager(Config::config config) : impl_(std::make_unique<Impl>(std::move(config))) {
}

Manager::~Manager() = default;

void Manager::load_drivers() const { impl_->load_drivers(); }

void Manager::validate_config() const { impl_->validate_config(); }

void Manager::install_signal_handler() const {
    impl_->install_signal_handler();
}

void Manager::run() const { impl_->run(); }
