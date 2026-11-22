#ifndef YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ENTRY_POINTS_H
#define YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ENTRY_POINTS_H

#include <yaddnsc/sdk/driver_abi.h>

struct SyncEntryPoints {
    decltype(&yaddnsc_driver_create) create = nullptr;
    decltype(&yaddnsc_driver_destroy) destroy = nullptr;
    decltype(&yaddnsc_driver_update) update = nullptr;
    decltype(&yaddnsc_driver_validate) validate = nullptr;
};

struct AsyncEntryPoints {
    decltype(&yaddnsc_driver_async_create) create = nullptr;
    decltype(&yaddnsc_driver_async_destroy) destroy = nullptr;
    decltype(&yaddnsc_driver_update_async_begin) begin = nullptr;
    decltype(&yaddnsc_driver_update_async_resume) resume = nullptr;
    decltype(&yaddnsc_driver_async_validate) validate = nullptr;
};

#endif  // YADDNSC_INFRASTRUCTURE_PLUGIN_DETAIL_ENTRY_POINTS_H
