#include "coro/detail/forward_list.hpp"

#include <coroutine>
#include <memory>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

namespace {
struct Node {
    int value;
    coro::detail::ForwardHook<Node*> first;
    coro::detail::ForwardHook<Node*> second;

    explicit Node(int id) : value(id) {
        first.bind(this);
        second.bind(this);
    }
};

using List = coro::detail::ForwardList<Node*>;
static_assert(!std::is_copy_constructible_v<List> && !std::is_move_constructible_v<List>);
static_assert(!std::is_copy_constructible_v<decltype(Node::first)>);
static_assert(!std::is_move_constructible_v<decltype(Node::first)>);

TEST(ForwardList, EmptyAndSingleNode_PopUnlinksAndAllowsReuse) {
    List list;
    Node node{1};
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.pop_front(), nullptr);
    list.push_back(node.first);
    EXPECT_FALSE(list.empty());
    EXPECT_TRUE(node.first.linked());
    EXPECT_EQ(list.pop_front(), &node);
    EXPECT_FALSE(node.first.linked());
    EXPECT_TRUE(list.empty());
    list.push_back(node.first);
    EXPECT_EQ(list.pop_front(), &node);
    EXPECT_TRUE(list.empty());
}

TEST(ForwardList, SeparateHooksAndSplicing_KeepMembershipsIndependent) {
    List first, second, target;
    Node a{1}, b{2}, c{3};
    first.push_back(a.first);
    first.push_back(b.first);
    second.push_back(a.second);
    target.push_back(c.first);
    target.splice_all(first);
    EXPECT_TRUE(first.empty());
    EXPECT_TRUE(a.first.linked());
    EXPECT_TRUE(a.second.linked());
    EXPECT_EQ(target.pop_front(), &c);
    EXPECT_EQ(target.pop_front(), &a);
    EXPECT_EQ(target.pop_front(), &b);
    EXPECT_TRUE(target.empty());
    EXPECT_FALSE(a.first.linked());
    EXPECT_EQ(second.pop_front(), &a);
    EXPECT_FALSE(a.second.linked());
    target.splice_all(first);
    EXPECT_TRUE(target.empty());
}

TEST(ForwardList, SpliceOntoEmpty_DetachesTheBatchFromLaterAppends) {
    List live, batch;
    Node a{1}, b{2}, c{3};
    live.push_back(a.first);
    live.push_back(b.first);
    batch.splice_all(live);
    EXPECT_TRUE(live.empty());
    live.push_back(c.first);
    EXPECT_FALSE(live.empty());
    EXPECT_EQ(batch.pop_front(), &a);
    EXPECT_EQ(batch.pop_front(), &b);
    EXPECT_EQ(batch.pop_front(), nullptr);
    EXPECT_EQ(live.pop_front(), &c);
    EXPECT_TRUE(batch.empty());
    EXPECT_TRUE(live.empty());
}

TEST(ForwardList, PopWalk_AppendsToTheLiveTail) {
    List list;
    Node a{1}, b{2}, c{3};
    list.push_back(a.first);
    list.push_back(b.first);
    std::vector<int> seen;
    while (Node* node = list.pop_front()) {
        seen.push_back(node->value);
        if (node == &a && seen.size() == 1) {
            list.push_back(c.first);
            list.push_back(a.first);
        }
    }
    EXPECT_EQ(seen, (std::vector<int>{1, 2, 3, 1}));
    EXPECT_TRUE(list.empty());
    EXPECT_FALSE(a.first.linked());
    EXPECT_FALSE(b.first.linked());
    EXPECT_FALSE(c.first.linked());
}

TEST(ForwardList, BorrowedFrameTokens_PopAndRebindPreserveTarget) {
    coro::detail::ForwardHook<std::coroutine_handle<>> hook;
    coro::detail::ForwardList<std::coroutine_handle<>> list;
    const std::coroutine_handle<> frame = std::noop_coroutine();
    hook.bind(frame);
    list.push_back(hook);
    EXPECT_TRUE(hook.linked());
    EXPECT_EQ(list.pop_front().address(), frame.address());
    EXPECT_FALSE(hook.linked());
    EXPECT_FALSE(list.pop_front());
    hook.bind(frame);
    list.push_back(hook);
    EXPECT_EQ(list.pop_front().address(), frame.address());
    EXPECT_TRUE(list.empty());
    frame.resume();
}

TEST(ForwardList, RebindingLinkedTarget_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            List list;
            Node first{1};
            Node second{2};
            list.push_back(first.first);
            first.first.bind(&second);
        },
        "");
}

TEST(ForwardList, EmptyFrameTokenInsertion_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            coro::detail::ForwardHook<std::coroutine_handle<>> hook;
            coro::detail::ForwardList<std::coroutine_handle<>> list;
            list.push_back(hook);
        },
        "");
}

TEST(ForwardList, DuplicateInsertion_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            List list;
            Node node{1};
            list.push_back(node.first);
            list.push_back(node.first);
        },
        "");
}

TEST(ForwardList, UnboundInsertion_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            List list;
            coro::detail::ForwardHook<Node*> hook;
            list.push_back(hook);
        },
        "");
}

TEST(ForwardList, LinkedNodeDestruction_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            List list;
            auto node = std::make_unique<Node>(1);
            list.push_back(node->first);
            node.reset();
        },
        "");
}

TEST(ForwardList, NonemptyListDestruction_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            Node node{1};
            auto list = std::make_unique<List>();
            list->push_back(node.first);
            list.reset();
        },
        "");
}

TEST(ForwardList, SelfSplice_TerminatesInEveryBuild) {
    EXPECT_DEATH(
        {
            List list;
            list.splice_all(list);
        },
        "");
}
}  // namespace
