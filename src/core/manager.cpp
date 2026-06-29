//
// Created by Kotarou on 2022/4/7.
//

#include "manager.h"

#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <csignal>
#include <filesystem>
#include <queue>
#include <stop_token>
#include <thread>
#include <utility>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>

#include "config/config_validator.hpp"
#include "dns/base.h"
#include "dns/classic.h"
#include "dns/doh.h"
#include "dns/dot.h"
#include "dns/multi_resolver.h"
#include "driver_manager.h"
#include "fmt.hpp"
#include "min_update_interval.h"
#include "network/http_client.h"
#include "network/inet_address.h"
#include "network/network_manager.h"
#include "update_task.h"
#include "updater.h"
#include "uri.h"
#include "util/algorithm.h"

// ---------------------------------------------------------------------------
// Manager::Impl — owns the shared thread pool, the scheduling data structures,
//                 the Updater, and all dependencies it requires.
// ---------------------------------------------------------------------------
class Manager::Impl {
public:
  explicit Impl(Config::config config) : config_(std::move(config)) {
    // Build the list of DNS servers from config, preserving backward
    // compatibility with the legacy single-server format.
    std::vector<dns_server_type> dns_servers;
    if (config_.resolver.use_custom_server) {
      if (!config_.resolver.servers.empty()) {
        dns_servers = config_.resolver.servers;
      } else if (!config_.resolver.address.empty()) {
        // Legacy single-server format.
        dns_servers.push_back(
            {config_.resolver.address, config_.resolver.port});
      }
    }

    // Build resolver objects from server configurations.
    // Each address is parsed as a URI to determine the protocol:
    //   https://...  → DohResolver (DNS-over-HTTPS, with a default
    //   PersistentHttpClient) tls://...    → DotResolver (DNS-over-TLS)
    //   otherwise    → ClassicResolver (traditional UDP/TCP DNS)
    std::vector<std::shared_ptr<ResolverBase>> resolvers;
    for (const auto &server : dns_servers) {
      const auto uri = Uri::parse(server.address);
      if (uri.get_schema() == "https") {
        auto http_client = std::make_unique<PersistentHttpClient>(
            uri,
            HttpClientOptions{.connection_timeout = std::chrono::seconds(1),
                              .read_timeout = std::chrono::seconds(5)});
        auto resolver = std::make_shared<DohResolver>(std::move(http_client),
                                                      server.address);
        SPDLOG_INFO("Using {}: {}", resolver->get_type(), server.address);
        resolvers.push_back(std::move(resolver));
      } else if (uri.get_schema() == "tls") {
        auto resolver = std::make_shared<DotResolver>(
            std::string(uri.get_host()), static_cast<uint16_t>(uri.get_port()));
        SPDLOG_INFO("Using {}: {}:{}", resolver->get_type(), uri.get_host(),
                    uri.get_port());
        resolvers.push_back(std::move(resolver));
      } else {
        auto resolver = std::make_shared<ClassicResolver>(server);
        SPDLOG_INFO("Using {}: {}:{}", resolver->get_type(), server.address,
                    server.port);
        resolvers.push_back(std::move(resolver));
      }
    }

    resolver_pool_ =
        MultiResolver(std::move(resolvers), config_.resolver.strategy);

    // Log configured custom resolver count and strategy — once at startup.
    if (dns_servers.size() > 1) {
      SPDLOG_INFO(
          "Configured {} custom resolver(s) in {} mode", dns_servers.size(),
          config_.resolver.strategy == Config::resolver_strategy::FALLBACK
              ? "fallback"
              : "concurrent");
    }

    updater_ = std::make_unique<Updater>(driver_manager_, network_manager_,
                                         resolver_pool_);
  }

  ~Impl() {
    // If the signal thread was started but is still blocked on sigwait()
    // (e.g. install_signal_handler() was called but run() never ran,
    // or an exception escaped run()), send SIGTERM to wake it up so
    // that ~jthread can join cleanly instead of hanging.
    //
    // In normal operation the signal thread has already exited by this
    // point (it called request_stop() and returned), so the kill() is
    // harmless — SIGTERM is blocked in all thread masks and nobody is
    // waiting on sigwait for it, so it is simply discarded.
    if (signal_thread_.joinable()) {
      kill(getpid(), SIGTERM);
    }
  }

  // --- driver loading ------------------------------------------------

  void load_drivers() {
    if (config_.driver.auto_discover) {
      // Auto-discover all .so files in driver_dir; manual load list is ignored
      if (!config_.driver.load.empty()) {
        SPDLOG_WARN("auto_discover is enabled, ignoring manual load list with "
                    "{} entry(ies)",
                    config_.driver.load.size());
      }

      const auto base_dir = std::filesystem::path(config_.driver.driver_dir);
      if (!std::filesystem::exists(base_dir)) {
        SPDLOG_WARN("auto_discover enabled but driver_dir '{}' does not exist",
                    config_.driver.driver_dir);
        return;
      }
      if (!std::filesystem::is_directory(base_dir)) {
        SPDLOG_WARN("auto_discover enabled but '{}' is not a directory",
                    config_.driver.driver_dir);
        return;
      }

      for (const auto &entry : std::filesystem::directory_iterator(base_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".so") {
          driver_manager_.load_driver(entry.path().string());
        }
      }
    } else {
      // Manual load list
      auto &load = config_.driver.load;
      Util::dedupe(load);

      const auto base_dir = std::filesystem::path(config_.driver.driver_dir);
      for (const auto &driver : load) {
        const auto driver_full_path = base_dir.empty()
                                          ? std::filesystem::path(driver)
                                          : base_dir / driver;
        driver_manager_.load_driver(driver_full_path.string());
      }
    }

    if (driver_manager_.get_loaded_drivers().empty()) {
      SPDLOG_WARN("No drivers were loaded, DDNS updates will not be performed");
    }
  }

  // --- config validation (delegated) ----------------------------------

  void validate_config() const {
    const ConfigValidator<YADDNSC_MIN_UPDATE_INTERVAL> validator(
        driver_manager_, network_manager_);
    validator.validate(config_);
  }

  // --- scheduling ---------------------------------------------------------

  void install_signal_handler() {
    // Idempotent: do nothing if the handler thread already exists or
    // has already been consumed (stop_source is one-shot).
    if (signal_thread_.joinable()) {
      return;
    }

    // Signal-handler thread.
    //
    // SIGINT and SIGTERM were blocked in the main thread before Manager
    // was constructed, so all threads inherited the blocked mask.  This
    // thread is the only thread that handles them via sigwait().  Once a
    // signal arrives it requests a graceful stop — the scheduler loop
    // in run() will break out and drain the thread pool before returning.
    signal_thread_ = std::jthread([this] {
      sigset_t sigset;
      sigemptyset(&sigset);
      sigaddset(&sigset, SIGINT);
      sigaddset(&sigset, SIGTERM);

      int sig;
      sigwait(&sigset, &sig);
      SPDLOG_INFO("Received exit signal, quitting...");
      if (!stop_source_.request_stop()) {
        SPDLOG_WARN("Stop request failed, current stop_possible: {}, "
                    "stop_requested: {}",
                    stop_source_.stop_possible(),
                    stop_source_.stop_requested());
      }
    });
  }

  void run() {
    // Resize the thread pool based on the total number of subdomains.
    pool_.reset(estimated_pool_size());

    // Print available interfaces.
    const auto interfaces = network_manager_.get_interfaces();
    SPDLOG_INFO("All available interfaces: {}", fmt::join(interfaces, ", "));

    // Log custom resolver info is handled by DnsResolver internally.

    // Build the initial schedule (one SubdomainEntry per subdomain).
    build_initial_schedule();

    auto st = stop_source_.get_token();

    // Register a stop callback that wakes up the scheduler immediately.
    std::stop_callback cb(st, [this] { cv_.notify_all(); });

    std::unique_lock lock(cv_mtx_);

    while (!st.stop_requested()) {
      const auto now = std::chrono::steady_clock::now();

      // Pop all entries whose deadline has passed.
      while (!schedule_.empty() && now >= schedule_.top().deadline) {
        auto entry = std::move(const_cast<SubdomainEntry &>(schedule_.top()));
        schedule_.pop();

        // Copy the task for the pool — the entry's copy must remain
        // valid for re-queuing into the schedule heap.
        auto update_task = entry.task;
        update_task.force_update = check_force_update(entry, now);

        // Submit a single subdomain update task to the shared pool.
        pool_.detach_task(
            [&updater = *updater_, task = std::move(update_task)] {
              try {
                updater.process(task);
              } catch (const std::exception &e) {
                SPDLOG_ERROR("Unhandled exception during update for {}: {}",
                             task.fqdn, e.what());
              }
            });

        // Re-schedule this entry for the next interval.
        entry.deadline = now + std::chrono::seconds(entry.update_interval);
        schedule_.push(std::move(entry));
      }

      // Sleep until the nearest deadline (or indefinitely if the heap is
      // empty).
      if (schedule_.empty()) {
        cv_.wait(lock, [&st] { return st.stop_requested(); });
      } else {
        cv_.wait_until(lock, schedule_.top().deadline,
                       [&st] { return st.stop_requested(); });
      }
    }

    // Gracefully drain all in-flight pool tasks before the Manager is torn
    // down.
    SPDLOG_INFO("Stop requested, waiting for {} pending task(s) to complete...",
                pool_.get_tasks_queued());
    pool_.wait();
    SPDLOG_INFO("All tasks completed.");
  }

private:
  // -----------------------------------------------------------------------
  // Heap helpers
  // -----------------------------------------------------------------------

  void build_initial_schedule() {
    domain_states_.reserve(config_.domains.size());

    for (size_t di = 0; di < config_.domains.size(); ++di) {
      const auto &[name, update_interval, force_update, driver, subdomains] =
          config_.domains[di];

      domain_states_.emplace_back();

      for (const auto &subdomain : subdomains) {
        const auto fqdn = fmt::format("{}.{}", subdomain.name, name);

        // Use per-subdomain update_interval if set (> 0), otherwise
        // fall back to the domain-level value.
        const auto effective_interval = subdomain.update_interval > 0
                                            ? subdomain.update_interval
                                            : update_interval;

        schedule_.push(SubdomainEntry{
            .deadline = std::chrono::steady_clock::now(),
            .update_interval = effective_interval,
            .force_update_interval = force_update,
            .domain_idx = di,
            .task =
                {
                    .subdomain = subdomain,
                    .domain_name = name,
                    .driver_name = driver,
                    .fqdn = fqdn,
                    .force_update = false,
                },
        });
      }
    }

    SPDLOG_INFO("Scheduler initialised with {} tasks", schedule_.size());
  }

  bool check_force_update(const SubdomainEntry &entry,
                          std::chrono::steady_clock::time_point now) {
    if (entry.force_update_interval <= 0) {
      return false;
    }

    auto &state = domain_states_[entry.domain_idx];
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                             now - state.last_force_update)
                             .count();

    if (elapsed >= entry.force_update_interval) {
      state.last_force_update = now;
      return true;
    }

    return false;
  }

  unsigned int estimated_pool_size() const {
    unsigned int total_subdomains = 0;
    const auto thread_count = std::thread::hardware_concurrency();

    for (const auto &domain : config_.domains) {
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

private:
  // ---- owned components --------------------------------------------------
  //
  // IMPORTANT: destruction order is the reverse of declaration order.
  // resolver_pool_ MUST outlive updater_ because Updater::Impl stores a
  // reference to it.  NetworkManager and DriverManager must also outlive
  // updater_ for the same reason.
  //
  DriverManager driver_manager_;
  NetworkManager network_manager_;
  MultiResolver resolver_pool_;
  std::unique_ptr<Updater> updater_;
  BS::thread_pool<> pool_;

  // stop_source_ must be declared before signal_thread_ so that during
  // destruction ~jthread() joins the signal thread before ~stop_source()
  // runs — any final request_stop() from the signal thread during cleanup
  // will see a valid stop_source.
  std::stop_source stop_source_;
  std::jthread signal_thread_;

  // ---- scheduling state --------------------------------------------------
  std::priority_queue<SubdomainEntry, std::vector<SubdomainEntry>,
                      std::greater<>>
      schedule_;
  std::vector<DomainState> domain_states_;

  // ---- synchronisation for scheduler sleep -------------------------------
  std::mutex cv_mtx_;
  std::condition_variable cv_;

  // ---- config & derived values -------------------------------------------
  Config::config config_;
};

// ---------------------------------------------------------------------------
// Manager public API
// ---------------------------------------------------------------------------

Manager::Manager(Config::config config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

Manager::~Manager() = default;

void Manager::load_drivers() const { impl_->load_drivers(); }

void Manager::validate_config() const { impl_->validate_config(); }

void Manager::install_signal_handler() const {
  impl_->install_signal_handler();
}

void Manager::run() const { impl_->run(); }
