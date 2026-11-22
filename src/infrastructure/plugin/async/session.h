#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_SESSION_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_SESSION_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <yaddnsc/sdk/driver_abi.h>

#include "domain/update/driver_update_command.h"
#include "infrastructure/plugin/async/http.h"
#include "infrastructure/plugin/async/package.h"
#include "infrastructure/plugin/async/services.h"
#include "infrastructure/plugin/async/step.h"
#include "infrastructure/plugin/detail/driver_session.h"
#include "infrastructure/plugin/detail/session_dependencies.h"

namespace plugin {

/// One async ABI protocol session. Only this object's owning coroutine mutates
/// ABI handle/phase state; its HTTP actor owns all network operation state.
class AsyncDriverSession final : public DriverSession {
public:
    AsyncDriverSession(std::shared_ptr<const AsyncPluginPackage> package, domain::DriverUpdateCommand command,
                       SessionDependencies dependencies);
    AsyncDriverSession(std::shared_ptr<const AsyncPluginPackage> package, std::string driver_param_json,
                       SessionDependencies dependencies);

    [[nodiscard]] coro::Task<DriverSessionOutcome> run() override;
    void request_cancel() noexcept override;
    [[nodiscard]] coro::Task<void> drain() override;

private:
    enum class Operation { UPDATE, VALIDATE };

    struct Cancellation final : AsyncCancellation {
        std::atomic<bool> flag{false};

        [[nodiscard]] bool cancelled() const noexcept override { return flag.load(std::memory_order_acquire); }
    };

    [[nodiscard]] coro::Task<DriverSessionOutcome> run_update();
    [[nodiscard]] coro::Task<DriverSessionOutcome> run_validation();
    [[nodiscard]] coro::Task<DriverSessionOutcome> execute_update();
    [[nodiscard]] coro::Task<bool> apply_commands(bool terminal);

    [[nodiscard]] detail::AsyncStepOutcome create_step();
    [[nodiscard]] detail::AsyncStepOutcome begin_step();
    [[nodiscard]] detail::AsyncStepOutcome resume_step(const std::shared_ptr<AsyncEventPacket>& event);
    [[nodiscard]] detail::AsyncStepOutcome destroy_step();
    [[nodiscard]] bool finish_step(const detail::AsyncStepOutcome& outcome, std::string_view entry,
                                   DriverSessionResult& result, bool& plugin_cancelled,
                                   std::optional<std::string_view> fqdn = std::nullopt);

    std::shared_ptr<const AsyncPluginPackage> package_;
    SessionDependencies dependencies_;
    std::shared_ptr<Cancellation> cancellation_;
    std::shared_ptr<AsyncHttpPump> pump_;
    HostAsyncServicesContext services_;
    AsyncStepCommands commands_;
    yaddnsc_async_driver* handle_{nullptr};
    std::optional<domain::DriverUpdateCommand> command_;
    std::string driver_param_json_;
    Operation operation_;
};

}  // namespace plugin

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_ASYNC_SESSION_H
