//
// Created by Kotarou on 2026/7/13.
//

#ifndef YADDNSC_DRIVER_XML_RAII_HPP
#define YADDNSC_DRIVER_XML_RAII_HPP

#include <memory>

#include <libxml/parser.h>
#include <libxml/xpath.h>

/// RAII wrappers for libxml2 C types.
///
/// These ensure that xmlDoc, xmlXPathContext, and xmlXPathObject are
/// automatically freed when they go out of scope, eliminating manual
/// cleanup and providing exception safety.
namespace xml_raii {

struct XmlDocDeleter {
    void operator()(xmlDoc* doc) const noexcept { xmlFreeDoc(doc); }
};

using unique_doc = std::unique_ptr<xmlDoc, XmlDocDeleter>;

struct XPathCtxDeleter {
    void operator()(xmlXPathContext* ctx) const noexcept { xmlXPathFreeContext(ctx); }
};

using unique_xpath_ctx = std::unique_ptr<xmlXPathContext, XPathCtxDeleter>;

struct XPathObjDeleter {
    void operator()(xmlXPathObject* obj) const noexcept { xmlXPathFreeObject(obj); }
};

using unique_xpath_obj = std::unique_ptr<xmlXPathObject, XPathObjDeleter>;

}  // namespace xml_raii

#endif  // YADDNSC_DRIVER_XML_RAII_HPP
