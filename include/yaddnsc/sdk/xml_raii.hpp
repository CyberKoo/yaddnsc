#ifndef YADDNSC_SDK_XML_RAII_HPP
#define YADDNSC_SDK_XML_RAII_HPP

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

/// RAII wrappers for libxml2 C types.
///
/// These ensure that xmlDoc, xmlXPathContext, and xmlXPathObject are
/// automatically freed when they go out of scope, eliminating manual
/// cleanup and providing exception safety.
namespace xml_raii {

struct XmlDocDeleter {
    void operator()(xmlDoc* doc) const noexcept { xmlFreeDoc(doc); }
};

using UniqueXmlDoc = std::unique_ptr<xmlDoc, XmlDocDeleter>;

struct XPathCtxDeleter {
    void operator()(xmlXPathContext* ctx) const noexcept { xmlXPathFreeContext(ctx); }
};

using UniqueXPathCtx = std::unique_ptr<xmlXPathContext, XPathCtxDeleter>;

struct XPathObjDeleter {
    void operator()(xmlXPathObject* obj) const noexcept { xmlXPathFreeObject(obj); }
};

using UniqueXPathObj = std::unique_ptr<xmlXPathObject, XPathObjDeleter>;

/// An XML document parsed and ready for XPath queries.
///
/// Owns the document and its XPath context, so the common provider-response
/// patterns — one text node at a path, or the text of every matching node —
/// do not repeat the parse/context/node-set plumbing at each call site.
class XmlDocument {
  public:
    /// Parse @p body. Returns nullopt when libxml2 rejects the document or
    /// the XPath context cannot be created.
    [[nodiscard]] static std::optional<XmlDocument> parse(std::string_view body) {
        UniqueXmlDoc doc(xmlReadMemory(body.data(), static_cast<int>(body.size()), nullptr, nullptr, 0));
        if (!doc) {
            return std::nullopt;
        }
        UniqueXPathCtx ctx(xmlXPathNewContext(doc.get()));
        if (!ctx) {
            return std::nullopt;
        }
        return XmlDocument(std::move(doc), std::move(ctx));
    }

    /// Register @p prefix for namespace @p uri so XPath expressions can use
    /// `prefix:element`. Returns false when registration fails.
    [[nodiscard]] bool register_ns(const char* prefix, const char* uri) const noexcept {
        return xmlXPathRegisterNs(ctx_.get(), reinterpret_cast<const xmlChar*>(prefix),
                                  reinterpret_cast<const xmlChar*>(uri)) == 0;
    }

    /// Text content of the first node matching @p xpath, if there is one.
    [[nodiscard]] std::optional<std::string> first_text(const char* xpath) const {
        const UniqueXPathObj result = eval(xpath);
        if (!result || result->nodesetval == nullptr || result->nodesetval->nodeNr == 0) {
            return std::nullopt;
        }
        xmlChar* text = xmlNodeGetContent(result->nodesetval->nodeTab[0]);
        if (text == nullptr) {
            return std::nullopt;
        }
        std::string value(reinterpret_cast<const char*>(text));
        xmlFree(text);
        return value;
    }

    /// Text content of every node matching @p xpath, in document order.
    [[nodiscard]] std::vector<std::string> all_text(const char* xpath) const {
        std::vector<std::string> values;
        const UniqueXPathObj result = eval(xpath);
        if (!result || result->nodesetval == nullptr) {
            return values;
        }
        for (int i = 0; i < result->nodesetval->nodeNr; ++i) {
            xmlChar* text = xmlNodeGetContent(result->nodesetval->nodeTab[i]);
            if (text != nullptr) {
                values.emplace_back(reinterpret_cast<const char*>(text));
                xmlFree(text);
            }
        }
        return values;
    }

    /// Raw XPath evaluation for traversals the text helpers do not cover.
    [[nodiscard]] UniqueXPathObj eval(const char* xpath) const {
        return UniqueXPathObj(xmlXPathEvalExpression(reinterpret_cast<const xmlChar*>(xpath), ctx_.get()));
    }

  private:
    XmlDocument(UniqueXmlDoc doc, UniqueXPathCtx ctx) : doc_(std::move(doc)), ctx_(std::move(ctx)) {}

    UniqueXmlDoc doc_;
    UniqueXPathCtx ctx_;
};

}  // namespace xml_raii

#endif  // YADDNSC_SDK_XML_RAII_HPP
