//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <memory>
#include <utility>

#include <spdlog/spdlog.h>

#include "updater.h"
#include "scheduler.h"
#include "dns/factory.h"
#include "driver_loader.h"
#include "dns/dispatcher.h"
#include "driver_manager.h"
#include "ip_source/factory.h"
#include "config/validator.hpp"
#include "min_update_interval.h"
#include "ip_source/iface_util.h"

// ---------------------------------------------------------------------------
// Manager::Impl — orchestrates the lifecycle of all subsystem components.
// ---------------------------------------------------------------------------

struct Manager::Impl {
    explicit Impl(Config::AppConfig config, std::stop_source stop_source);

    void load_drivers();

    void validate_config() const;

    void run();

    // IMPORTANT: destruction order is the reverse of declaration order.
    // config_ is declared first because it's needed by dispatcher_'s constructor.
    Config::AppConfig config_;
    DriverManager driver_manager_;
    ResolverDispatcher dispatcher_;
    Updater updater_;
    Scheduler scheduler_;
};

Manager::Impl::Impl(Config::AppConfig config, std::stop_source stop_source)
    : config_(std::move(config)),
      dispatcher_(DnsResolverFactory::create(config_)),
      updater_(driver_manager_, dispatcher_),
      scheduler_(config_, updater_, std::move(stop_source)) {
}

void Manager::Impl::load_drivers() {
    DriverLoader::load(driver_manager_, config_);
}

void Manager::Impl::validate_config() const {
    const auto interfaces = InterfaceUtil::get_interfaces();
    const ConfigValidator<YADDNSC_MIN_UPDATE_INTERVAL> validator(driver_manager_, interfaces);
    validator.validate(config_);
}

void Manager::Impl::run() {
    const auto interfaces = InterfaceUtil::get_interfaces();
    SPDLOG_INFO("All available interfaces: {}", fmt::join(interfaces, ", "));

    scheduler_.run();
}

// ---------------------------------------------------------------------------
// Manager public API
// ---------------------------------------------------------------------------

Manager::Manager(Config::AppConfig config, std::stop_source stop_source)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(stop_source))) {
}

Manager::~Manager() = default;

void Manager::load_drivers() const { impl_->load_drivers(); }

void Manager::validate_config() const { impl_->validate_config(); }

void Manager::run() const { impl_->run(); }
