#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <unordered_map>

#include "PiSubmarine/Lease/Api/ILeaseIssuer.h"
#include "PiSubmarine/Lease/Api/ILeaseSecretProvider.h"
#include "PiSubmarine/Lease/Api/ILeaseValidator.h"
#include "PiSubmarine/Lease/Api/IResourceRegistry.h"
#include "PiSubmarine/Logging/Api/IFactory.h"

namespace PiSubmarine::Lease::InMemory
{
    class Manager
        : public Api::ILeaseIssuer
        , public Api::ILeaseSecretProvider
        , public Api::ILeaseValidator
        , public Api::IResourceRegistry
    {
    public:
        using TimePoint = std::chrono::steady_clock::time_point;
        using Clock = std::function<TimePoint()>;
        using LeaseIdGenerator = std::function<Error::Api::Result<Api::LeaseId>()>;
        using LeaseSecretGenerator = std::function<Error::Api::Result<Api::LeaseSecret>()>;

        // Thread-safe.
        explicit Manager(
            Logging::Api::IFactory& loggerFactory,
            Clock clock = [] { return std::chrono::steady_clock::now(); },
            LeaseIdGenerator leaseIdGenerator = nullptr,
            LeaseSecretGenerator leaseSecretGenerator = nullptr);

        [[nodiscard]] Error::Api::Result<Api::LeaseGrant> AcquireLease(const Api::LeaseRequest& request) override;
        [[nodiscard]] Error::Api::Result<Api::Lease> RenewLease(const Api::LeaseId& leaseId) override;
        [[nodiscard]] Error::Api::Result<void> ReleaseLease(const Api::LeaseId& leaseId) override;
        [[nodiscard]] Error::Api::Result<Api::LeaseSecret> GetLeaseSecret(const Api::LeaseId& leaseId) const override;
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
            Api::LeaseSecret Secret;
            TimePoint ExpiresAt;
        };

        [[nodiscard]] static Error::Api::Result<Api::LeaseId> GenerateSecureLeaseId();
        [[nodiscard]] static Error::Api::Result<Api::LeaseSecret> GenerateSecureLeaseSecret();
        [[nodiscard]] static Error::Api::Result<void> ValidateResourceId(const Api::ResourceId& resourceId);
        [[nodiscard]] static Error::Api::Result<void> ValidateLeaseId(const Api::LeaseId& leaseId);
        [[nodiscard]] static Error::Api::Result<void> ValidateLeaseSecret(const Api::LeaseSecret& leaseSecret);
        [[nodiscard]] static Error::Api::Result<void> ValidateResourceDescriptor(const Api::ResourceDescriptor& resource);
        void PurgeExpiredLeasesLocked(const TimePoint& now) const;
        [[nodiscard]] std::size_t CountActiveLeasesForResourceLocked(const std::string& resourceId) const;

        Clock m_Clock;
        LeaseIdGenerator m_LeaseIdGenerator;
        LeaseSecretGenerator m_LeaseSecretGenerator;
        std::shared_ptr<spdlog::logger> m_Logger;

        mutable std::mutex m_Mutex;
        mutable std::unordered_map<std::string, ActiveLease> m_ActiveLeases;
        std::unordered_map<std::string, Api::ResourceDescriptor> m_Resources;
    };
}
