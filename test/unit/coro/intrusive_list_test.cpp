#include "coro/detail/intrusive_list.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

namespace {
struct Node {
    int value;
    coro::detail::IntrusiveHook<Node> first;
    coro::detail::IntrusiveHook<Node> second;

    explicit Node(int id) : value(id) {
        first.bind(*this);
        second.bind(*this);
    }
};

using List = coro::detail::IntrusiveList<Node>;
static_assert(!std::is_copy_constructible_v<List> && !std::is_move_constructible_v<List>);
static_assert(!std::is_copy_constructible_v<decltype(Node::first)>);
static_assert(!std::is_move_constructible_v<decltype(Node::first)>);

TEST(IntrusiveList, EmptyAndSingleNode_PopUnlinksAndAllowsReuse) {
    List list;
    Node node{1};
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.front(), nullptr);
    EXPECT_EQ(list.pop_front(), nullptr);
    list.push_back(node.first);
    EXPECT_EQ(list.front(), &node);
    EXPECT_TRUE(node.first.linked());
    EXPECT_EQ(node.first.next(), nullptr);
    EXPECT_EQ(list.pop_front(), &node);
    EXPECT_FALSE(node.first.linked());
    node.first.unlink();
    node.first.unlink();
    list.push_front(node.first);
    EXPECT_EQ(list.pop_front(), &node);
    EXPECT_TRUE(list.empty());
}

TEST(IntrusiveList, MixedInsertionAndHeadMiddleTailRemoval_PreservesRing) {
    List list;
    Node a{1}, b{2}, c{3}, d{4};
    list.push_back(b.first);
    list.push_front(a.first);
    list.push_back(c.first);
    list.push_back(d.first);
    EXPECT_EQ(a.first.next(), &b);
    EXPECT_EQ(b.first.next(), &c);
    EXPECT_EQ(c.first.next(), &d);
    b.first.unlink();
    a.first.unlink();
    d.first.unlink();
    EXPECT_EQ(list.front(), &c);
    EXPECT_EQ(c.first.next(), nullptr);
    EXPECT_EQ(list.pop_front(), &c);
    EXPECT_TRUE(list.empty());
    list.push_back(a.first);
    list.push_back(b.first);
    EXPECT_EQ(list.pop_front(), &a);
    EXPECT_EQ(list.pop_front(), &b);
}

TEST(IntrusiveList, SeparateHooks_KeepMembershipsIndependent) {
    List first, second;
    Node a{1}, b{2};
    first.push_back(a.first);
    first.push_back(b.first);
    second.push_back(a.second);
    EXPECT_EQ(first.pop_front(), &a);
    EXPECT_FALSE(a.first.linked());
    EXPECT_TRUE(a.second.linked());
    EXPECT_EQ(first.pop_front(), &b);
    EXPECT_TRUE(first.empty());
    EXPECT_EQ(second.pop_front(), &a);
    EXPECT_FALSE(a.second.linked());
    EXPECT_TRUE(second.empty());
}

TEST(IntrusiveList, PopCallback_RemovesSuccessorWithoutLosingTail) {
    List list;
    Node a{1}, b{2}, c{3};
    list.push_back(a.first);
    list.push_back(b.first);
    list.push_back(c.first);
    std::vector<int> seen;
    while (auto* node = list.pop_front()) {
        EXPECT_FALSE(node->first.linked());
        seen.push_back(node->value);
        if (node == &a)
            b.first.unlink();
    }
    EXPECT_EQ(seen, (std::vector<int>{1, 3}));
}

TEST(IntrusiveList, DuplicateInsertion_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            List list;
            Node node{1};
            list.push_back(node.first);
            list.push_front(node.first);
        },
        "");
}

TEST(IntrusiveList, LinkedNodeDestruction_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            List list;
            auto node = std::make_unique<Node>(1);
            list.push_back(node->first);
            node.reset();
        },
        "");
}

TEST(IntrusiveList, NonemptyListDestruction_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            Node node{1};
            auto list = std::make_unique<List>();
            list->push_back(node.first);
            list.reset();
        },
        "");
}

TEST(IntrusiveList, UnlinkedTraversal_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            Node node{1};
            [[maybe_unused]] auto* next = node.first.next();
        },
        "");
}
}  // namespace
