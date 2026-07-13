//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRV_ALIBABA_CLOUD_RESPONSE_HPP
#define YADDNSC_DRV_ALIBABA_CLOUD_RESPONSE_HPP

#include <string>
#include <optional>
#include <glaze/glaze.hpp>

/// Alibaba Cloud DNS success response for UpdateDomainRecord.
struct AlibabaUpdateResponse {
    std::string request_id;   ///< Request ID (maps to "RequestId")
    std::string record_id;    ///< Updated record ID (maps to "RecordId")
};

/// Alibaba Cloud DNS error response.
struct AlibabaErrorResponse {
    std::string code;              ///< Error code (maps to "Code")
    std::string message;           ///< Error message (maps to "Message")
    std::optional<std::string> request_id;  ///< Request ID (maps to "RequestId")
    std::optional<std::string> host_id;     ///< Host ID (maps to "HostId")
};

template<>
struct glz::meta<AlibabaUpdateResponse> {
    using T = AlibabaUpdateResponse;
    static constexpr auto value = object(
        "RequestId", &T::request_id,
        "RecordId", &T::record_id
    );
};

template<>
struct glz::meta<AlibabaErrorResponse> {
    using T = AlibabaErrorResponse;
    static constexpr auto value = object(
        "Code", &T::code,
        "Message", &T::message,
        "RequestId", &T::request_id,
        "HostId", &T::host_id
    );
};

#endif // YADDNSC_DRV_ALIBABA_CLOUD_RESPONSE_HPP
