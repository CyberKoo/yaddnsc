// Cancellation tree traversal: bounded stack, stable ordering and shielding.
#include <cstdlib>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <sys/resource.h>

#include "coro/cancel_scope.h"
#include "coro/detail/access.h"
#include "coro/detail/wait_operation.h"

namespace {

// The caller reserves enough order entries before cancellation. Recording is
// therefore allocation-free, as required by the subscription's noexcept hook.
class WaitProbe {
public:
    WaitProbe(coro::CancelScope& scope, std::vector<int>& order, int tag) : order_(&order), tag_(tag) {
        node.arm(scope, this, [](void* context) noexcept {
            auto& probe = *static_cast<WaitProbe*>(context);
            probe.cancelled = true;
            probe.order_->push_back(probe.tag_);
            return coro::detail::CancelAction::WAIT_EXTERNAL;
        });
    }

    WaitProbe(const WaitProbe&) = delete;
    WaitProbe& operator=(const WaitProbe&) = delete;

    ~WaitProbe() noexcept { node.disarm(); }

    coro::detail::CancelSubscription node{};
    bool cancelled = false;

private:
    std::vector<int>* order_;
    int tag_;
};

TEST(CancelTree, BranchTraversal_PreservesPreorderWaiterOrderAndShieldBoundaries) {
    coro::CancelScope root;
    coro::CancelScope first{&root};
    coro::CancelScope leaf{&first};
    coro::CancelScope shield{&first, true};
    coro::CancelScope shield_leaf{&shield};
    coro::CancelScope after_shield{&first};
    coro::CancelScope second{&root};
    coro::CancelScope second_leaf{&second};
    coro::CancelScope tail_shield{&root, true};
    coro::CancelScope tail{&root};
    std::vector<int> order;
    order.reserve(11);
    WaitProbe root_wait{root, order, 0};
    WaitProbe later_root_wait{root, order, 9};
    WaitProbe first_wait{first, order, 1};
    WaitProbe leaf_wait{leaf, order, 2};
    WaitProbe shield_wait{shield, order, 3};
    WaitProbe shield_leaf_wait{shield_leaf, order, 30};
    WaitProbe after_shield_wait{after_shield, order, 4};
    WaitProbe second_wait{second, order, 5};
    WaitProbe second_leaf_wait{second_leaf, order, 6};
    WaitProbe tail_shield_wait{tail_shield, order, 7};
    WaitProbe tail_wait{tail, order, 8};

    root.cancel();
    EXPECT_EQ(order, (std::vector<int>{9, 0, 1, 2, 4, 5, 6, 8}));
    EXPECT_TRUE(tail_wait.cancelled);
    EXPECT_TRUE(leaf_wait.cancelled);
    EXPECT_FALSE(leaf_wait.node.armed());
    EXPECT_TRUE(after_shield.cancelled());
    EXPECT_FALSE(after_shield.timed_out());
    EXPECT_FALSE(shield.cancelled());
    EXPECT_FALSE(shield_leaf.cancelled());
    EXPECT_FALSE(shield_wait.cancelled);
    EXPECT_TRUE(shield_leaf_wait.node.armed());
    EXPECT_FALSE(tail_shield_wait.cancelled);
    root.cancel();
    EXPECT_EQ(order.size(), 8u);  // idempotence must not duplicate hooks/wakeups

    shield.cancel();  // a shield's own cancellation still reaches its children
    tail_shield.cancel();
    EXPECT_EQ(order, (std::vector<int>{9, 0, 1, 2, 4, 5, 6, 8, 3, 30, 7}));
    EXPECT_TRUE(shield_leaf_wait.cancelled);
    EXPECT_FALSE(shield_leaf_wait.node.armed());
}

TEST(CancelTree, ChildCancellation_DoesNotWalkIntoAncestorsOrTheirOtherChildren) {
    coro::CancelScope root;
    coro::CancelScope branch{&root};
    coro::CancelScope leaf{&branch};
    coro::CancelScope sibling{&root};
    std::vector<int> order;
    order.reserve(4);
    WaitProbe root_wait{root, order, 0};
    WaitProbe branch_wait{branch, order, 1};
    WaitProbe leaf_wait{leaf, order, 2};
    WaitProbe sibling_wait{sibling, order, 3};
    branch.cancel();
    EXPECT_EQ(order, (std::vector<int>{1, 2}));
    EXPECT_FALSE(root.cancelled());
    EXPECT_FALSE(root_wait.cancelled);
    EXPECT_FALSE(sibling_wait.cancelled);
    root.cancel();
    EXPECT_EQ(order, (std::vector<int>{1, 2, 0, 3}));
}

TEST(CancelTree, RemovingHeadMiddleAndTail_PreservesRemainingSiblingLinks) {
    coro::CancelScope root;
    auto head = std::make_unique<coro::CancelScope>(&root);
    auto middle = std::make_unique<coro::CancelScope>(&root);
    auto first_live = std::make_unique<coro::CancelScope>(&root);
    auto second_live = std::make_unique<coro::CancelScope>(&root);
    auto tail = std::make_unique<coro::CancelScope>(&root);
    middle.reset();
    head.reset();
    tail.reset();
    auto appended = std::make_unique<coro::CancelScope>(&root);
    std::vector<int> order;
    order.reserve(3);
    {
        WaitProbe first_wait{*first_live, order, 1};
        WaitProbe second_wait{*second_live, order, 2};
        WaitProbe appended_wait{*appended, order, 3};
        root.cancel();
        EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    }
    first_live.reset();
    second_live.reset();
    appended.reset();
    const coro::CancelScope late_child{&root};  // reuse an emptied child list
    EXPECT_TRUE(late_child.cancelled());
}

TEST(CancelTree, ConstObserverSiblings_AllowMembershipUpdates) {
    coro::CancelScope root;
    const coro::CancelScope first{&root};
    auto middle = std::make_unique<coro::CancelScope>(&root);
    const coro::CancelScope last{&root};
    middle.reset();
    root.cancel();
    EXPECT_TRUE(first.cancelled());
    EXPECT_TRUE(last.cancelled());
}

// Ensure even allocation failure during chain setup destroys descendants first.
struct ScopeChain {
    ScopeChain() = default;
    ScopeChain(const ScopeChain&) = delete;
    ScopeChain& operator=(const ScopeChain&) = delete;

    ~ScopeChain() noexcept {
        while (!scopes.empty()) {
            scopes.pop_back();
        }
    }

    std::vector<std::unique_ptr<coro::CancelScope>> scopes;
};

bool cancel_deep_chain() {
    constexpr std::size_t DEPTH = 100000;
    coro::CancelScope root;
    ScopeChain chain;
    chain.scopes.reserve(DEPTH);
    coro::CancelScope* parent = &root;
    for (std::size_t i = 0; i < DEPTH; ++i) {
        chain.scopes.push_back(std::make_unique<coro::CancelScope>(parent));
        parent = chain.scopes.back().get();
    }
    std::vector<int> order;
    order.reserve(1);
    WaitProbe leaf_wait{*parent, order, 42};
    root.cancel();
    root.cancel();
    return leaf_wait.cancelled && !leaf_wait.node.armed() && parent->cancelled() && !parent->timed_out() &&
           order == std::vector<int>{42};
}

TEST(CancelTree, DeepChain_CancelsWithoutGrowingTheNativeStack) {
    // A stack-overflow regression must fail this case, not kill the whole suite.
    EXPECT_EXIT(
        {
            // Hosts with unlimited stacks must not hide a recursive traversal.
            struct rlimit limit{};
            if (::getrlimit(RLIMIT_STACK, &limit) != 0) {
                std::_Exit(EXIT_FAILURE);
            }
            constexpr rlim_t STACK_BUDGET = 1024U * 1024U;
            if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > STACK_BUDGET) {
                limit.rlim_cur = STACK_BUDGET;
            }
            if (::setrlimit(RLIMIT_STACK, &limit) != 0) {
                std::_Exit(EXIT_FAILURE);
            }
            std::_Exit(cancel_deep_chain() ? EXIT_SUCCESS : EXIT_FAILURE);
        },
        ::testing::ExitedWithCode(EXIT_SUCCESS), "");
}

TEST(CancelTree, DestroyingParentWithLiveChild_FailsExplicitly) {
    EXPECT_DEATH(
        {
            auto parent = std::make_unique<coro::CancelScope>();
            const coro::CancelScope child{parent.get()};
            parent.reset();
        },
        "");
}

static_assert(std::is_nothrow_constructible_v<coro::CancelScope, coro::CancelScope*, bool>);

}  // namespace
