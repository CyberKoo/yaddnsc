//
// ResolverDispatcher unit tests — jthread-based concurrent dispatch.
//
// Compiled with the jthread-based dispatcher.cpp.  See
// test/fixtures/dispatcher_tests.h for the shared test bodies.
// =============================================================================

#include "fixtures/dispatcher_tests.h"

// ===========================================================================
//  Additional branch coverage — fallback / concurrent edge cases
// ===========================================================================

TEST(DispatcherFallback, MultiAddressResult_ReturnsAllRecords) {
    // A resolver returning more than one address exercises the >1 warning.
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _)).WillByDefault(Return(make_multi_a_response(0x1234)));
    ON_CALL(*r1, query(_, _)).WillByDefault(Return(err(DnsError::RETRY, "r1")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::FALLBACK);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 2U);
}

TEST(DispatcherFallback, FirstResolverSucceeds_DoesNotQueryLater) {
    // FALLBACK must honour configured order: a successful primary must not
    // consult later resolvers. This would flake if the walk were shuffled.
    auto r0 = make_mock();
    auto r1 = make_mock();
    EXPECT_CALL(*r0, query(_, _)).WillOnce(Return(ok_a()));
    EXPECT_CALL(*r1, query(_, _)).Times(0);
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::FALLBACK);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(DispatcherFallback, ServerRefused_ThenSuccess) {
    // SERVER_REFUSED is retryable in fallback mode → try the next resolver.
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _)).WillByDefault(Return(err(DnsError::SERVER_REFUSED, "r0")));
    ON_CALL(*r1, query(_, _)).WillByDefault(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::FALLBACK);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(DispatcherShuffle, AnySucceeds_ReturnsRecords) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _)).WillByDefault(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::SHUFFLE);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(DispatcherShuffle, NxDomain_StopsIteration) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _)).WillByDefault(Return(err(DnsError::NX_DOMAIN, "nxdomain")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::SHUFFLE);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_unexpected(result, DnsError::NX_DOMAIN);
}

TEST(DispatcherShuffle, AllRetryable_ReturnsTransientFailure) {
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _)).WillByDefault(Return(err(DnsError::RETRY, "r0")));
    ON_CALL(*r1, query(_, _)).WillByDefault(Return(err(DnsError::RETRY, "r1")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::SHUFFLE);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_transient_failure(result);
}

TEST(DispatcherConcurrent, AllParseErrors_ReturnsParseError) {
    // Definitive (PARSE) errors stop the batch with that error.
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _)).WillByDefault(Return(err(DnsError::PARSE, "r0")));
    ON_CALL(*r1, query(_, _)).WillByDefault(Return(err(DnsError::PARSE, "r1")));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    expect_unexpected(result, DnsError::PARSE);
}

TEST(DispatcherConcurrent, TwoResolversBothSucceed) {
    // Both succeed concurrently — the second set_promise_value hits the
    // future_error catch (only one promise value is delivered).
    auto r0 = make_mock();
    auto r1 = make_mock();
    ON_CALL(*r0, query(_, _)).WillByDefault(Return(ok_a()));
    ON_CALL(*r1, query(_, _)).WillByDefault(Return(ok_a()));
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    resolvers.push_back(std::move(r0));
    resolvers.push_back(std::move(r1));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}

TEST(DispatcherConcurrent, BatchesAcrossResolvers) {
    // More than MAX_CONCURRENT_RESOLVERS (3) → multiple batches; the resolver
    // in the second batch provides the answer.
    std::vector<std::unique_ptr<ResolverBase>> resolvers;
    for (int i = 0; i < 3; ++i) {
        auto r = make_mock();
        ON_CALL(*r, query(_, _)).WillByDefault(Return(err(DnsError::RETRY, "r" + std::to_string(i))));
        resolvers.push_back(std::move(r));
    }
    auto r_last = make_mock();
    ON_CALL(*r_last, query(_, _)).WillByDefault(Return(ok_a()));
    resolvers.push_back(std::move(r_last));
    ResolverDispatcher disp(std::move(resolvers), Config::ResolverStrategy::CONCURRENT);

    auto result = disp.resolve("example.com", RecordKind::A, 1, 1);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ((*result)[0], "192.168.1.1");
}
