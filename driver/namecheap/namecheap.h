//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_NAMECHEAP_NAMECHEAP_H
#define YADDNSC_DRV_NAMECHEAP_NAMECHEAP_H

#include <string>

#include <yaddnsc/sdk/driver.hpp>

#include "config.hpp"

/// Namecheap Dynamic DNS driver for updating A records.
///
/// Implements the Namecheap DDNS HTTPS API via a simple GET-based update
/// endpoint.  The API only supports IPv4 (A records); AAAA records are
/// unsupported by the upstream service.
///
/// API reference:
///   https://www.namecheap.com/support/knowledgebase/article.aspx/29/11/how-to-configure-your-dns-dynamic-dns-update-url/
class NamecheapDriver final : public yaddnsc::sdk::Driver {
public:
    ~NamecheapDriver() override = default;

    /// Perform one update: generate-request → HTTP exchange → check-response.
    yaddnsc::sdk::Result update(yaddnsc::sdk::UpdateContext &context) override;

    /// Validate driver_param against the Namecheap API schema without
    /// updating; schema violations surface as YADDNSC_STATUS_INVALID_CONFIG.
    yaddnsc::sdk::Result validate(std::string_view driver_param_json) const override;

private:
    /// Build the GET request for a Namecheap DDNS update.
    ///
    /// The `host` parameter uses the subdomain label directly; for a
    /// bare-domain (apex) record the configuration should pass "@".
    static yaddnsc::sdk::HttpRequest generate_request(const NamecheapParams &cfg,
                                                      const yaddnsc::sdk::UpdateRequest &params);

    /// Validate the Namecheap API response XML using libxml2.
    static bool check_response(const yaddnsc::sdk::HttpResponse &response, const yaddnsc::sdk::Services &services);
};

#endif  // YADDNSC_DRV_NAMECHEAP_NAMECHEAP_H
