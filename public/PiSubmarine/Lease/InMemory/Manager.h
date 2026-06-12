#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>

#include "PiSubmarine/Lease/Api/ILeaseIssuer.h"
#include "PiSubmarine/Lease/Api/ILeaseValidator.h"
#include "PiSubmarine/Lease/Api/IResourceRegistry.h"
#include "PiSubmarine/Logging/Api/IFactory.h"

namespace PiSubmarine::Lease::InMemory
{
    class Manager : public Api::ILeaseIssuer, public Api::ILeaseValidator, public Api::IResourceRegistry
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;
        using Clock = std::function<TimePoint()>;
        using LeaseIdGenerator = std::function<Error::Api::Result<Api::LeaseId>()>;

        // Thread-safe.
        explicit Manager(
            Logging::Api::IFactory& loggerFactory,
            Clock clock = [] { return std::chrono::steady_clock::now(); },
            LeaseIdGenerator leaseIdGenerator = {});

        [[nodiscard]] Error::Api::Result<Api::Lease> AcquireLease(const Api::LeaseRequest& request) override;
        [[nodiscard]] Error::Api::Result<Api::Lease> RenewLease(const Api::LeaseId& leaseId) override;
        [[nodiscard]] Error::Api::Result<void> ReleaseLease(const Api::LeaseId& leaseId) override;
        [[nodiscard]] Error::Api::Result<Api::LeaseValidation> ValidateLease(
            const Api::LeaseId& leaseId,
            const Api::ResourceId& resourceId) const override;

        [[nodiscard]] Error::Api::Result<void> RegisterResource(const Api::ResourceDescriptor& resource) override;
        [[nodiscard]] Error::Api::Result<Api::ResourceDescriptor> GetResource(
            const Api::ResourceId& resourceId) const override;

    private:
        struct ActiveLease
        {
            Api::Lease Lease;
            TimePoint ExpiresAt;
        };

        [[nodiscard]] static Error::Api::Result<Api::LeaseId> GenerateSecureLeaseId();
        [[nodiscard]] static Error::Api::Result<void> ValidateResourceId(const Api::ResourceId& resourceId);
        [[nodiscard]] static Error::Api::Result<void> ValidateLeaseId(const Api::LeaseId& leaseId);
        [[nodiscard]] static Error::Api::Result<void> ValidateResourceDescriptor(const Api::ResourceDescriptor& resource);
        void PurgeExpiredLeasesLocked(const TimePoint& now) const;
        [[nodiscard]] std::size_t CountActiveLeasesForResourceLocked(const std::string& resourceId) const;

        Clock m_Clock;
        LeaseIdGenerator m_LeaseIdGenerator;
        std::shared_ptr<spdlog::logger> m_Logger;

        mutable std::mutex m_Mutex;
        mutable std::unordered_map<std::string, ActiveLease> m_ActiveLeases;
        std::unordered_map<std::string, Api::ResourceDescriptor> m_Resources;
    };
}
