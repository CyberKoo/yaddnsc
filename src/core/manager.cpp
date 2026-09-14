//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <memory>
#include <thread>
#include <utility>

#include "config/config.h"
#include "config/validator.hpp"
#include "dns/dispatcher.h"
#include "dns/factory.h"
#include "http_client/client.h"
#include "http_client/stream_factory.h"
#include "ip_source/base.h"
#include "ip_source/factory.h"
#include "ip_source/iface_util.h"
#include "util/cancellation_token.hpp"
#include "version.h"

#include "driver_loader.h"
#include "driver_manager.h"
#include "scheduler.h"
#include "updater.h"
#include "update_task.hpp"
#include "min_update_interval.h"
#include "exception/driver_not_found.h"

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>

namespace {
    std::uint32_t estimate_pool_size(const Config::AppConfig &config) noexcept {
        std::uint32_t total_subdomains = 0;
        const auto thread_count = std::thread::hardware_concurrency();

        for (const auto &domain: config.domains) {
            total_subdomains += static_cast<std::uint32_t>(domain.subdomains.size());
        }

        if (total_subdomains < 2 || thread_count < 2) {
            return 2;
        }

        if (total_subdomains < thread_count) {
            return total_subdomains;
        }

        return std::min(thread_count, 4U);
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Manager::Impl — orchestrates the lifecycle of all subsystem components.
// ---------------------------------------------------------------------------

struct Manager::Impl {
    explicit Impl(Config::AppConfig config, std::stop_source stop_source);

    Impl(Config::AppConfig config, std::stop_source stop_source, ResolverDispatcher dispatcher,
         HttpClientFactory http_factory);

    void load_drivers();

    void validate_config() const;

    void run();

    // IMPORTANT: destruction order is the reverse of declaration order.
    // config_ is declared first because it's needed by dispatcher_'s constructor.
    // cancel_src_ is declared before every token consumer (updater_, the
    // factories) so it outlives them all.
    std::shared_ptr<const Config::AppConfig> config_;
    std::shared_ptr<Utils::CancellationSource> cancel_src_;
    DriverManager driver_manager_;
    ResolverDispatcher dispatcher_;
    Updater updater_;
    BS::thread_pool<> thread_pool_;
    Scheduler scheduler_;
    std::stop_source stop_source_;
    std::unique_ptr<std::stop_callback<std::function<void()>>> stop_cb_;
    HttpClientFactory http_client_factory_;
};

namespace {
    /// IP-source factory bound to the manager's cancellation source: every
    /// HTTP IP source created from it is cancellable through the same token.
    [[nodiscard]] Updater::IpSourceFactory make_ip_source_factory(const std::shared_ptr<Utils::CancellationSource> &src) {
        return [src](const Config::SubdomainConfig &cfg) { return IpSourceFactory::create(cfg, src->token()); };
    }
} // namespace

Manager::Impl::Impl(Config::AppConfig config, std::stop_source stop_source)
    : config_(std::make_shared<const Config::AppConfig>(std::move(config))),
      cancel_src_(std::make_shared<Utils::CancellationSource>()),
      dispatcher_(DnsResolverFactory::create(*config_, cancel_src_->token())), updater_(dispatcher_, make_ip_source_factory(cancel_src_)),
      thread_pool_(estimate_pool_size(*config_)), scheduler_(config_, stop_source.get_token()),
      stop_source_(std::move(stop_source)) {
    stop_cb_ = std::make_unique<std::stop_callback<std::function<void()>>>(
        stop_source_.get_token(), [src = cancel_src_] { src->trigger(); });

    http_client_factory_ = [src = cancel_src_] {
        net::http::Options opts;
        opts.user_agent = YADDNSC::get_full_version();
        return std::make_unique<net::http::Client>(std::move(opts), src->token());
    };
}

Manager::Impl::Impl(Config::AppConfig config, std::stop_source stop_source, ResolverDispatcher dispatcher,
                    HttpClientFactory http_factory)
    : config_(std::make_shared<const Config::AppConfig>(std::move(config))),
      cancel_src_(std::make_shared<Utils::CancellationSource>()),
      dispatcher_(std::move(dispatcher)), updater_(dispatcher_, make_ip_source_factory(cancel_src_)),
      thread_pool_(estimate_pool_size(*config_)), scheduler_(config_, stop_source.get_token()),
      stop_source_(std::move(stop_source)), http_client_factory_(std::move(http_factory)) {
    stop_cb_ = std::make_unique<std::stop_callback<std::function<void()>>>(
        stop_source_.get_token(), [src = cancel_src_] { src->trigger(); });
}

void Manager::Impl::load_drivers() {
    DriverLoader::load(driver_manager_, *config_);
}

void Manager::Impl::validate_config() const {
    const auto interfaces = InterfaceUtil::get_interfaces();
    const ConfigValidator<YADDNSC_MIN_UPDATE_INTERVAL> validator(driver_manager_.get_loaded_drivers(), interfaces);
    validator.validate(*config_);
}

void Manager::Impl::run() {
    const auto interfaces = InterfaceUtil::get_interfaces();
    SPDLOG_INFO("All available interfaces: {}", fmt::join(interfaces, ", "));

    while (!stop_source_.stop_requested()) {
        auto tasks = scheduler_.pop_all_due();

        for (auto &task: tasks) {
            try {
                auto driver = &driver_manager_.get_driver(std::string(task.driver_name()));
                thread_pool_.detach_task([this, driver, t = std::move(task)] {
                    auto http_client = http_client_factory_();
                    updater_.process(t, *driver, *http_client);
                });
            } catch (const DriverNotFoundException &e) {
                SPDLOG_ERROR("Driver '{}' not found for task '{}', skipping: {}", task.driver_name(), task.fqdn,
                             e.what());
            }
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

Manager::Manager(Config::AppConfig config, std::stop_source stop_source, ResolverDispatcher dispatcher,
                 HttpClientFactory http_factory)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(stop_source), std::move(dispatcher),
                                   std::move(http_factory))) {
}

Manager::~Manager() = default;

void Manager::load_drivers() {
    impl_->load_drivers();
}

void Manager::validate_config() const {
    impl_->validate_config();
}

void Manager::run() {
    impl_->run();
}
