#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>

#include "PiSubmarine/Lease/InMemory/Manager.h"
#include "PiSubmarine/Lease/Api/ErrorCode.h"
#include "PiSubmarine/Logging/Api/IFactory.h"

namespace PiSubmarine::Lease::InMemory
{
    namespace
    {
        using namespace std::chrono_literals;

        [[nodiscard]] Api::ResourceDescriptor MakeExclusiveResource()
        {
            return Api::ResourceDescriptor{
                .Id = Api::ResourceId{"control-main"},
                .Policy = Api::LeasePolicy{
                    .MaxLeases = 1,
                    .LeaseDuration = 5s}};
        }

        class LoggerFactoryStub final : public Logging::Api::IFactory
        {
        public:
            [[nodiscard]] std::shared_ptr<spdlog::logger> CreateLogger(std::string_view name) override
            {
                return std::make_shared<spdlog::logger>(
                    std::string(name),
                    std::make_shared<spdlog::sinks::null_sink_mt>());
            }
        };
    }

    TEST(ManagerTest, RegisterResourceRejectsInvalidPolicy)
    {
        LoggerFactoryStub loggerFactory;
        Manager manager(loggerFactory);

        const auto result = manager.RegisterResource(Api::ResourceDescriptor{
            .Id = Api::ResourceId{"control-main"},
            .Policy = Api::LeasePolicy{
                .MaxLeases = 0,
                .LeaseDuration = 5s}});

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().Cause, make_error_code(Api::ErrorCode::InvalidMaxLeases));
    }

    TEST(ManagerTest, GetResourceReturnsRegisteredDescriptor)
    {
        LoggerFactoryStub loggerFactory;
        Manager manager(loggerFactory);
        const auto descriptor = MakeExclusiveResource();

        ASSERT_TRUE(manager.RegisterResource(descriptor).has_value());

        const auto result = manager.GetResource(descriptor.Id);

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, descriptor);
    }

    TEST(ManagerTest, AcquireLeaseGeneratesHexEncodedOpaqueId)
    {
        LoggerFactoryStub loggerFactory;
        Manager manager(loggerFactory);
        ASSERT_TRUE(manager.RegisterResource(MakeExclusiveResource()).has_value());

        const auto lease = manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"control-main"}});

        ASSERT_TRUE(lease.has_value());
        EXPECT_EQ(lease->Id.Value.size(), 64U);
        EXPECT_TRUE(std::all_of(
            lease->Id.Value.begin(),
            lease->Id.Value.end(),
            [](const char value) { return std::isxdigit(static_cast<unsigned char>(value)) != 0; }));
    }

    TEST(ManagerTest, AcquireLeaseRejectsSecondExclusiveLease)
    {
        LoggerFactoryStub loggerFactory;
        Manager manager(loggerFactory);
        ASSERT_TRUE(manager.RegisterResource(MakeExclusiveResource()).has_value());
        ASSERT_TRUE(manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"control-main"}}).has_value());

        const auto result = manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"control-main"}});

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().Cause, make_error_code(Api::ErrorCode::LeaseLimitReached));
    }

    TEST(ManagerTest, AcquireLeaseAllowsSharedLeasesUpToConfiguredLimit)
    {
        std::size_t nextId = 0;
        LoggerFactoryStub loggerFactory;
        Manager manager(
            loggerFactory,
            [] { return std::chrono::steady_clock::time_point{}; },
            [&nextId]() -> Error::Api::Result<Api::LeaseId>
            {
                ++nextId;
                return Api::LeaseId{.Value = "lease-" + std::to_string(nextId)};
            });
        ASSERT_TRUE(manager.RegisterResource(Api::ResourceDescriptor{
            .Id = Api::ResourceId{"telemetry-main"},
            .Policy = Api::LeasePolicy{
                .MaxLeases = 2,
                .LeaseDuration = 5s}}).has_value());

        ASSERT_TRUE(manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"telemetry-main"}}).has_value());
        ASSERT_TRUE(manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"telemetry-main"}}).has_value());

        const auto thirdLease = manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"telemetry-main"}});

        ASSERT_FALSE(thirdLease.has_value());
        EXPECT_EQ(thirdLease.error().Cause, make_error_code(Api::ErrorCode::LeaseLimitReached));
    }

    TEST(ManagerTest, RenewLeaseExtendsExpiration)
    {
        auto now = std::chrono::steady_clock::time_point{};
        std::size_t nextId = 0;
        LoggerFactoryStub loggerFactory;
        Manager manager(
            loggerFactory,
            [&now] { return now; },
            [&nextId]() -> Error::Api::Result<Api::LeaseId>
            {
                ++nextId;
                return Api::LeaseId{.Value = "lease-" + std::to_string(nextId)};
            });
        ASSERT_TRUE(manager.RegisterResource(MakeExclusiveResource()).has_value());

        const auto lease = manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"control-main"}});
        ASSERT_TRUE(lease.has_value());

        now += 4s;
        ASSERT_TRUE(manager.RenewLease(lease->Id).has_value());

        now += 4s;
        const auto validationAfterRenew = manager.ValidateLease(lease->Id, Api::ResourceId{"control-main"});

        ASSERT_TRUE(validationAfterRenew.has_value());
        EXPECT_TRUE(validationAfterRenew->IsValid);

        now += 2s;
        const auto validationAfterExpiry = manager.ValidateLease(lease->Id, Api::ResourceId{"control-main"});

        ASSERT_TRUE(validationAfterExpiry.has_value());
        EXPECT_FALSE(validationAfterExpiry->IsValid);
    }

    TEST(ManagerTest, ValidateLeaseRejectsWrongResourceAndExpiredLeaseWithoutTrustingCaller)
    {
        auto now = std::chrono::steady_clock::time_point{};
        LoggerFactoryStub loggerFactory;
        Manager manager(
            loggerFactory,
            [&now] { return now; },
            []() -> Error::Api::Result<Api::LeaseId> { return Api::LeaseId{.Value = "forged-resistant-id"}; });
        ASSERT_TRUE(manager.RegisterResource(MakeExclusiveResource()).has_value());
        ASSERT_TRUE(manager.RegisterResource(Api::ResourceDescriptor{
            .Id = Api::ResourceId{"video-main"},
            .Policy = Api::LeasePolicy{
                .MaxLeases = std::nullopt,
                .LeaseDuration = 5s}}).has_value());

        const auto lease = manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"control-main"}});
        ASSERT_TRUE(lease.has_value());

        const auto wrongResource = manager.ValidateLease(lease->Id, Api::ResourceId{"video-main"});
        ASSERT_TRUE(wrongResource.has_value());
        EXPECT_FALSE(wrongResource->IsValid);

        now += 6s;
        const auto expired = manager.ValidateLease(lease->Id, Api::ResourceId{"control-main"});
        ASSERT_TRUE(expired.has_value());
        EXPECT_FALSE(expired->IsValid);
    }

    TEST(ManagerTest, ReleaseLeaseRemovesActiveLease)
    {
        LoggerFactoryStub loggerFactory;
        Manager manager(
            loggerFactory,
            [] { return std::chrono::steady_clock::time_point{}; },
            []() -> Error::Api::Result<Api::LeaseId> { return Api::LeaseId{.Value = "lease-1"}; });
        ASSERT_TRUE(manager.RegisterResource(MakeExclusiveResource()).has_value());

        const auto lease = manager.AcquireLease(Api::LeaseRequest{.Resource = Api::ResourceId{"control-main"}});
        ASSERT_TRUE(lease.has_value());
        ASSERT_TRUE(manager.ReleaseLease(lease->Id).has_value());

        const auto validation = manager.ValidateLease(lease->Id, Api::ResourceId{"control-main"});

        ASSERT_TRUE(validation.has_value());
        EXPECT_FALSE(validation->IsValid);
    }
}
