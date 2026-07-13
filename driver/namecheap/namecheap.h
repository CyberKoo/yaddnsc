//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_NAMECHEAP_NAMECHEAP_H
#define YADDNSC_DRV_NAMECHEAP_NAMECHEAP_H

#include "driver/base.h"

/// Namecheap Dynamic DNS driver for updating A records.
///
/// Implements the Namecheap DDNS HTTPS API via a simple GET-based update
/// endpoint.  The API only supports IPv4 (A records); AAAA records are
/// unsupported by the upstream service.
///
/// API reference:
///   https://www.namecheap.com/support/knowledgebase/article.aspx/29/11/how-to-configure-your-dns-dynamic-dns-update-url/
class NamecheapDriver final : public BaseDriver {
public:
    ~NamecheapDriver() override = default;

    /// Build the API request from config and update params.
    ///
    /// @throws ParamParseException  When attempting to update an AAAA record
    ///                              (unsupported by the Namecheap DDNS API).
    [[nodiscard]] DriverRequestContext generate_request(const DriverConfig& config,
                                                        const DriverUpdateParams& ctx) const override;

    /// Validate the Namecheap API response XML using libxml2.
    [[nodiscard]] bool check_response(const HttpResponse& response) const override;

    /// Return static metadata about this driver.
    [[nodiscard]] DriverDetail get_detail() const noexcept override;
};

#endif  // YADDNSC_DRV_NAMECHEAP_NAMECHEAP_H
