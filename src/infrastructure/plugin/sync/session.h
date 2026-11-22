#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_SESSION_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_SESSION_H

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "domain/update/driver_update_command.h"
#include "infrastructure/plugin/detail/driver_session.h"
#include "infrastructure/plugin/detail/session_dependencies.h"
#include "infrastructure/plugin/sync/bridge.h"
#include "infrastructure/plugin/sync/package.h"

namespace plugin {

/// One synchronous update or validation cycle. The run coroutine owns its
/// worker and bridge nursery through real retirement; drain is therefore an
/// idempotent no-op after run returns or throws.
class SyncDriverSession final : public DriverSession {
public:
    SyncDriverSession(std::shared_ptr<const SyncPluginPackage> package, domain::DriverUpdateCommand command,
                      SessionDependencies dependencies);
    SyncDriverSession(std::shared_ptr<const SyncPluginPackage> package, std::string driver_param_json,
                      SessionDependencies dependencies);

    [[nodiscard]] coro::Task<DriverSessionOutcome> run() override;
    void request_cancel() noexcept override;
    [[nodiscard]] coro::Task<void> drain() override;

private:
    enum class Operation { UPDATE, VALIDATE };

    struct BridgeState final : BridgeSession {
        coro::NurseryToken lifetime;
        std::vector<std::weak_ptr<BridgeCall>> calls;
        std::atomic<bool> cancelled_flag{false};
        coro::CancelScope* worker_scope{nullptr};

        [[nodiscard]] coro::NurseryToken admission_token() const override { return lifetime; }

        [[nodiscard]] bool cancelled() const noexcept override {
            return cancelled_flag.load(std::memory_order_acquire);
        }

        void register_call(const std::shared_ptr<BridgeCall>& call) override;
        void unregister_call(const std::shared_ptr<BridgeCall>& call) noexcept override;
    };

    [[nodiscard]] DriverSessionResult run_operation();
    [[nodiscard]] DriverSessionResult run_update();
    [[nodiscard]] DriverSessionResult run_validation();
    void cancel_calls() noexcept;

    std::shared_ptr<const SyncPluginPackage> package_;
    SessionDependencies dependencies_;
    std::shared_ptr<Bridge> bridge_;
    std::shared_ptr<BridgeState> state_;
    std::optional<domain::DriverUpdateCommand> command_;
    std::string driver_param_json_;
    Operation operation_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_SYNC_SESSION_H
