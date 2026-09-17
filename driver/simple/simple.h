//
// Created by Kotarou on 2022/4/5.
//

#ifndef YADDNSC_DRV_SIMPLE_SIMPLE_H
#define YADDNSC_DRV_SIMPLE_SIMPLE_H

#include <yaddnsc/sdk/driver.hpp>

/// Simple HTTP GET driver for DNS record updates.
///
/// The simplest driver implementation — it embeds the IP address into
/// the URL or request body and doesn't parse the response beyond checking
/// the HTTP status code.
class SimpleDriver final : public yaddnsc::sdk::Driver {
public:
    ~SimpleDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

private:
    /// Build the HTTP GET request with the IP address embedded in the URL template.
    static yaddnsc::sdk::HttpRequest generate_request(const yaddnsc::sdk::UpdateRequest &params);

    /// Validate the response — returns true for 2xx status codes.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif //YADDNSC_DRV_SIMPLE_SIMPLE_H
