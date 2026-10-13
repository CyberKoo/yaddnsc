#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <yaddnsc/sdk/xml_raii.hpp>

TEST(XmlRaii, ReadsFirstAndAllTextAndRejectsOversizedInputBeforeAccess) {
    auto document = xml_raii::XmlDocument::parse("<root><item>one</item><item>two</item></root>");
    ASSERT_TRUE(document);
    EXPECT_EQ(document->first_text("/root/item"), "one");
    EXPECT_EQ(document->all_text("/root/item"), (std::vector<std::string>{"one", "two"}));
    EXPECT_FALSE(document->first_text("/root/missing"));
    EXPECT_TRUE(document->all_text("/root/missing").empty());
    const std::string_view oversized("x", static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1);
    EXPECT_FALSE(xml_raii::XmlDocument::parse(oversized));
}
