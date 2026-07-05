//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <memory>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>
#include <BS_thread_pool.hpp>

#include "updater.h"
#include "scheduler.h"
#include "update_task.h"
#include "dns/factory.h"
#include "driver_loader.h"
#include "dns/dispatcher.h"
#include "driver_manager.h"
#include "ip_source/factory.h"
#include "config/validator.hpp"
#include "min_update_interval.h"
#include "network/http_client.h"
#include "ip_source/iface_util.h"

namespace {
    unsigned int estimate_pool_size(const Config::AppConfig &config) {
        unsigned int total_subdomains = 0;
        const auto thread_count = std::thread::hardware_concurrency();

        for (const auto &domain: config.domains) {
            total_subdomains += static_cast<unsigned int>(domain.subdomains.size());
        }

        if (total_subdomains < 2 || thread_count < 2) {
            return 2;
        }

        if (total_subdomains < thread_count) {
            return total_subdomains;
        }

        return thread_count;
    }
} // anonymous namespace

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
    BS::thread_pool<> thread_pool_;
    Scheduler scheduler_;
    std::stop_source stop_source_;
};

Manager::Impl::Impl(Config::AppConfig config, std::stop_source stop_source)
    : config_(std::move(config)),
      dispatcher_(DnsResolverFactory::create(config_)),
      updater_(dispatcher_),
      thread_pool_(estimate_pool_size(config_)),
      scheduler_(config_, stop_source.get_token()),
      stop_source_(std::move(stop_source)) {
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

    while (!stop_source_.stop_requested()) {
        auto tasks = scheduler_.pop_all_due();

        for (auto &task: tasks) {
            auto driver = &driver_manager_.get_driver(task.driver_name);
            thread_pool_.detach_task(
                [this, driver, t = std::move(task)] {
                    TransientHttpClient http_client{};
                    updater_.process(t, *driver, http_client);
                });
        }

        if (!scheduler_.wait_for_next()) {
            break;
        }
    }

    thread_pool_.wait();
    SPDLOG_INFO("All tasks drained, shutting down");
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
