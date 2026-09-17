//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <memory>
#include <thread>
#include <utility>

#include "domain/config/runtime_config.h"
#include "domain/update/schedule_queue.h"
#include "config/validator.hpp"
#include "dns/dispatcher.h"
#include "dns/factory.h"
#include "dns/resolver_catalog.h"
#include "http_client/client.h"
#include "ip_source/adapter.h"
#include "ip_source/iface_util.h"
#include "util/cancellation_token.hpp"
#include "version.h"

#include "application/pool_task_executor.h"
#include "application/scheduler_runner.h"
#include "application/update_workflow.h"

#include "driver_loader.h"
#include "driver_manager.h"
#include "spdlog_logger.h"
#include "steady_clock.h"

#include "fmt.hpp"

namespace {
    std::uint32_t estimate_pool_size(const domain::RuntimeConfig &config) noexcept {
        std::uint32_t total_subdomains = 0;
        const auto thread_count = std::thread::hardware_concurrency();

        for (const auto &domain_config: config.domains) {
            total_subdomains += static_cast<std::uint32_t>(domain_config.subdomains.size());
        }

        if (total_subdomains < 2 || thread_count < 2) {
            return 2;
        }

        if (total_subdomains < thread_count) {
            return total_subdomains;
        }

        return std::min(thread_count, 4U);
    }

    /// HTTP client factory bound to the manager's cancellation source: every
    /// client created from it is cancellable through the same token.
    [[nodiscard]] HttpClientFactory make_http_client_factory(const std::shared_ptr<Utils::CancellationSource> &src) {
        return [src] {
            net::http::Options opts;
            opts.user_agent = YADDNSC::get_full_version();
            return std::make_unique<net::http::Client>(std::move(opts), src->token());
        };
    }
} // anonymous namespace

// ---------------------------------------------------------------------------
// Manager::Impl — composition root (Phase 3 shell): owns every component and
// drives the explicit shutdown sequence; the update policy itself lives in
// the domain/application components (ScheduleQueue, SchedulerRunner,
// UpdateWorkflow, TaskExecutor).
// ---------------------------------------------------------------------------

struct Manager::Impl {
    explicit Impl(domain::RuntimeConfig config, std::stop_source stop_source);

    Impl(domain::RuntimeConfig config, std::stop_source stop_source, ResolverDispatcher dispatcher,
         HttpClientFactory http_factory);

    void load_drivers();

    void validate_config() const;

    void run();

    // IMPORTANT: destruction order is the reverse of declaration order.
    // config_ is declared first because it's needed by dispatcher_'s constructor.
    // cancel_src_ is declared before every token consumer (the IP source
    // adapter, the HTTP client factory inside driver_gateway_) so it outlives
    // them all. task_executor_ is declared after the components its tasks
    // reference (workflow_, and transitively the ports) so its destructor
    // drains the pool before any of them — and long before driver_manager_
    // unloads the modules — can be destroyed.
    std::shared_ptr<const domain::RuntimeConfig> config_;
    std::shared_ptr<Utils::CancellationSource> cancel_src_;
    SpdlogLogger logger_;
    DriverManager driver_manager_;
    ResolverDispatcher dispatcher_;
    IpSourceAdapter ip_source_;
    CppDriverGateway driver_gateway_;
    UpdateWorkflow workflow_;
    SteadyClock clock_;
    domain::ScheduleQueue schedule_queue_;
    PoolTaskExecutor task_executor_;
    SchedulerRunner scheduler_runner_;
    std::stop_source stop_source_;
    std::unique_ptr<std::stop_callback<std::function<void()>>> stop_cb_;
};

Manager::Impl::Impl(domain::RuntimeConfig config, std::stop_source stop_source)
    : config_(std::make_shared<const domain::RuntimeConfig>(std::move(config))),
      cancel_src_(std::make_shared<Utils::CancellationSource>()),
      dispatcher_(DnsResolverFactory::create(config_->resolver, cancel_src_->token(), ResolverCatalog::with_builtins())),
      ip_source_(cancel_src_->token()), driver_gateway_(driver_manager_, make_http_client_factory(cancel_src_)),
      workflow_(dispatcher_, ip_source_, driver_gateway_, logger_), clock_(),
      schedule_queue_(config_, clock_.now()), task_executor_(estimate_pool_size(*config_), workflow_),
      scheduler_runner_(schedule_queue_, clock_, task_executor_, stop_source.get_token(), logger_),
      stop_source_(std::move(stop_source)) {
    stop_cb_ = std::make_unique<std::stop_callback<std::function<void()>>>(
        stop_source_.get_token(), [src = cancel_src_] { src->trigger(); });
}

Manager::Impl::Impl(domain::RuntimeConfig config, std::stop_source stop_source, ResolverDispatcher dispatcher,
                    HttpClientFactory http_factory)
    : config_(std::make_shared<const domain::RuntimeConfig>(std::move(config))),
      cancel_src_(std::make_shared<Utils::CancellationSource>()), dispatcher_(std::move(dispatcher)),
      ip_source_(cancel_src_->token()), driver_gateway_(driver_manager_, std::move(http_factory)),
      workflow_(dispatcher_, ip_source_, driver_gateway_, logger_), clock_(),
      schedule_queue_(config_, clock_.now()), task_executor_(estimate_pool_size(*config_), workflow_),
      scheduler_runner_(schedule_queue_, clock_, task_executor_, stop_source.get_token(), logger_),
      stop_source_(std::move(stop_source)) {
    stop_cb_ = std::make_unique<std::stop_callback<std::function<void()>>>(
        stop_source_.get_token(), [src = cancel_src_] { src->trigger(); });
}

void Manager::Impl::load_drivers() {
    DriverLoader::load(driver_manager_, config_->driver);
}

void Manager::Impl::validate_config() const {
    const auto interfaces = InterfaceUtil::get_interfaces();
    const EnvironmentValidator validator(driver_manager_.get_loaded_drivers(), interfaces);
    validator.validate(*config_);
}

void Manager::Impl::run() {
    const auto interfaces = InterfaceUtil::get_interfaces();
    YLOG_INFO(logger_, "All available interfaces: {}", fmt::join(interfaces, ", "));

    // Explicit shutdown sequence (refactor/phase-3-scheduling-and-workflow.md
    // §3.6), not just member declaration order:
    //   1. stop is requested through stop_source_ (e.g. by SignalWatcher);
    //   2. the runner stops popping new tasks and returns;
    scheduler_runner_.run();
    //   3. the executor stops accepting new tasks;
    task_executor_.shutdown();
    //   4. I/O cancellation fires via stop_cb_ (stop → CancellationSource),
    //      aborting in-flight blocking I/O;
    //   5. in-flight updates drain before any driver instance may be
    //      destroyed or module unloaded.
    task_executor_.wait_idle();
    YLOG_INFO(logger_, "All tasks drained, shutting down");
}

// ---------------------------------------------------------------------------
// Manager public API
// ---------------------------------------------------------------------------

Manager::Manager(domain::RuntimeConfig config, std::stop_source stop_source)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(stop_source))) {
}

Manager::Manager(domain::RuntimeConfig config, std::stop_source stop_source, ResolverDispatcher dispatcher,
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
