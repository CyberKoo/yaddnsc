#include "xml_payload.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libxml/tree.h>
#include <yaddnsc/sdk/xml_raii.hpp>

namespace route53 {
namespace {

/// XPath for the change status of a success response.
constexpr const char* XPATH_STATUS = "//r53:ChangeResourceRecordSetsResponse/r53:ChangeInfo/r53:Status/text()";

/// XPaths for the error entries of an error response.
constexpr const char* XPATH_ERROR_CODE = "//r53:Error/r53:Code/text()";
constexpr const char* XPATH_ERROR_MESSAGE = "//r53:Error/r53:Message/text()";

/// Namespace prefix used for every Route 53 XPath expression.
constexpr const char* NS_PREFIX = "r53";

}  // anonymous namespace

std::string build_change_body(const std::string& fqdn, std::string_view rd_type, std::string_view ip_addr, int ttl) {
    // Build the UPSERT XML document using libxml2's tree API.
    // This ensures proper XML escaping, namespace handling, and encoding.
    xml_raii::UniqueXmlDoc doc(xmlNewDoc(BAD_CAST "1.0"));
    if (!doc)
        return {};

    xmlNodePtr root = xmlNewDocNode(doc.get(), nullptr, BAD_CAST "ChangeResourceRecordSetsRequest", nullptr);
    if (!root)
        return {};
    xmlDocSetRootElement(doc.get(), root);

    // Register the default namespace on the root element.
    xmlNsPtr ns = xmlNewNs(root, BAD_CAST R53_XMLNS, nullptr);
    if (!ns)
        return {};
    xmlSetNs(root, ns);

    // Build the nested element hierarchy.
    xmlNodePtr batch = xmlNewChild(root, ns, BAD_CAST "ChangeBatch", nullptr);
    xmlNodePtr changes = xmlNewChild(batch, ns, BAD_CAST "Changes", nullptr);
    xmlNodePtr change = xmlNewChild(changes, ns, BAD_CAST "Change", nullptr);

    xmlNewTextChild(change, ns, BAD_CAST "Action", BAD_CAST "UPSERT");

    xmlNodePtr rrset = xmlNewChild(change, ns, BAD_CAST "ResourceRecordSet", nullptr);
    xmlNewTextChild(rrset, ns, BAD_CAST "Name", BAD_CAST fqdn.c_str());

    // xmlNewTextChild requires null-terminated strings; string_view::data() is not.
    const auto rd_type_str = std::string(rd_type);
    xmlNewTextChild(rrset, ns, BAD_CAST "Type", BAD_CAST rd_type_str.c_str());

    auto ttl_str = std::to_string(ttl);
    xmlNewTextChild(rrset, ns, BAD_CAST "TTL", BAD_CAST ttl_str.c_str());

    xmlNodePtr records = xmlNewChild(rrset, ns, BAD_CAST "ResourceRecords", nullptr);
    xmlNodePtr record = xmlNewChild(records, ns, BAD_CAST "ResourceRecord", nullptr);

    const auto ip_addr_str = std::string(ip_addr);
    xmlNewTextChild(record, ns, BAD_CAST "Value", BAD_CAST ip_addr_str.c_str());

    // Serialise the document to a string.
    xmlChar* xml_buf = nullptr;
    int xml_len = 0;
    xmlDocDumpMemory(doc.get(), &xml_buf, &xml_len);

    if (!xml_buf)
        return {};

    std::string result(reinterpret_cast<const char*>(xml_buf), static_cast<std::size_t>(xml_len));
    xmlFree(xml_buf);
    return result;
}

ChangeResponse parse_change_response(std::string_view body) {
    const auto doc = xml_raii::XmlDocument::parse(body);
    if (!doc)
        return {};

    if (!doc->register_ns(NS_PREFIX, R53_XMLNS))
        return {.parsed = true};

    return {.parsed = true, .namespace_registered = true, .status = doc->first_text(XPATH_STATUS)};
}

std::vector<ApiError> parse_error_response(std::string_view body) {
    const auto doc = xml_raii::XmlDocument::parse(body);
    if (!doc || !doc->register_ns(NS_PREFIX, R53_XMLNS))
        return {};

    const auto codes = doc->all_text(XPATH_ERROR_CODE);
    const auto messages = doc->all_text(XPATH_ERROR_MESSAGE);

    // <Code> and <Message> are both present in every Route 53 <Error>; pair them
    // by position and keep an entry for whichever side is there.
    const auto entry_count = std::max(codes.size(), messages.size());
    std::vector<ApiError> errors;
    errors.reserve(entry_count);
    for (std::size_t i = 0; i < entry_count; ++i) {
        ApiError error;
        if (i < codes.size())
            error.code = codes[i];
        if (i < messages.size())
            error.message = messages[i];
        errors.push_back(std::move(error));
    }
    return errors;
}

}  // namespace route53
