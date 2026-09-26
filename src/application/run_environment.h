//
// Created by Kotarou on 2026/9/27.
//

#ifndef YADDNSC_APPLICATION_RUN_ENVIRONMENT_H
#define YADDNSC_APPLICATION_RUN_ENVIRONMENT_H

#include <stop_token>

#include "application/ports/clock.h"
#include "application/ports/log.h"
#include "application/ports/network_interfaces.h"
#include "application/ports/task_executor.h"
#include "support/util/cancellation_token.hpp"

/// Cohesive port bundles for the run command, grouped by who uses them
/// together. Members are non-owning references (plus the value stop_source)
/// whose referents live in the composition root for the whole run.
///
/// These are NOT a general service context: a member that some holder does
/// not use means the bundle is mis-grouped and must be split, not padded.

/// ShutdownSignals — the two shutdown primitives that must stay paired:
/// the scheduler-level stop source (stop popping new tasks) and the
/// I/O-level cancellation source (abort in-flight blocking I/O). A stop
/// requested through the former triggers the latter.
struct ShutdownSignals {
    std::stop_source stop;
    const Utils::CancellationSource &cancellation;
};

/// RunEnvironment — the ambient services the run lifecycle draws on:
/// time, task execution, interface enumeration and logging.
struct RunEnvironment {
    Clock &clock;
    TaskExecutor &executor;
    const NetworkInterfaces &interfaces;
    const Logger &logger;
};

#endif // YADDNSC_APPLICATION_RUN_ENVIRONMENT_H
