#include "PiSubmarine/Lease/InMemory/Manager.h"

#include <algorithm>
#include <array>
#include <string_view>

#include <openssl/rand.h>

#include "PiSubmarine/Error/Api/MakeError.h"
#include "PiSubmarine/Lease/Api/ErrorCode.h"

namespace PiSubmarine::Lease::InMemory
{
    namespace
    {
        constexpr std::size_t LeaseIdByteCount = 32;
        constexpr std::size_t MaxLeaseIdGenerationAttempts = 8;

        [[nodiscard]] Error::Api::Error MakeContractError(const Api::ErrorCode code) noexcept
        {
            return Error::Api::MakeError(Error::Api::ErrorCondition::ContractError, make_error_code(code));
        }

        [[nodiscard]] Error::Api::Error MakeDeviceError(const Api::ErrorCode code) noexcept
        {
            return Error::Api::MakeError(Error::Api::ErrorCondition::DeviceError, make_error_code(code));
        }
    }

    Manager::Manager(Clock clock, LeaseIdGenerator leaseIdGenerator)
        : m_Clock(std::move(clock))
        , m_LeaseIdGenerator(std::move(leaseIdGenerator))
    {
        if (!m_LeaseIdGenerator)
        {
            m_LeaseIdGenerator = [] { return GenerateSecureLeaseId(); };
        }
    }

    Error::Api::Result<Api::Lease> Manager::AcquireLease(const Api::LeaseRequest& request)
    {
        if (const auto resourceValidation = ValidateResourceId(request.Resource); !resourceValidation)
        {
            return std::unexpected(resourceValidation.error());
        }

        const auto now = m_Clock();
        std::scoped_lock lock(m_Mutex);
        PurgeExpiredLeasesLocked(now);

        const auto resourceIterator = m_Resources.find(request.Resource.Value);
        if (resourceIterator == m_Resources.end())
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::ResourceNotFound));
        }

        const auto& resource = resourceIterator->second;
        if (resource.Policy.MaxLeases.has_value() &&
            CountActiveLeasesForResourceLocked(request.Resource.Value) >= resource.Policy.MaxLeases.value())
        {
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseLimitReached));
        }

        for (std::size_t attempt = 0; attempt < MaxLeaseIdGenerationAttempts; ++attempt)
        {
            const auto leaseIdResult = m_LeaseIdGenerator();
            if (!leaseIdResult)
            {
                return std::unexpected(leaseIdResult.error());
            }

            if (const auto leaseIdValidation = ValidateLeaseId(*leaseIdResult); !leaseIdValidation)
            {
                return std::unexpected(leaseIdValidation.error());
            }

            if (m_ActiveLeases.contains(leaseIdResult->Value))
            {
                continue;
            }

            ActiveLease activeLease{
                .Lease = Api::Lease{
                    .Id = *leaseIdResult,
                    .Resource = request.Resource,
                    .Duration = resource.Policy.LeaseDuration},
                .ExpiresAt = now + resource.Policy.LeaseDuration};
            m_ActiveLeases.emplace(activeLease.Lease.Id.Value, activeLease);
            return activeLease.Lease;
        }

        return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseIdGenerationFailed));
    }

    Error::Api::Result<Api::Lease> Manager::RenewLease(const Api::LeaseId& leaseId)
    {
        if (const auto leaseIdValidation = ValidateLeaseId(leaseId); !leaseIdValidation)
        {
            return std::unexpected(leaseIdValidation.error());
        }

        const auto now = m_Clock();
        std::scoped_lock lock(m_Mutex);

        const auto leaseIterator = m_ActiveLeases.find(leaseId.Value);
        if (leaseIterator == m_ActiveLeases.end())
        {
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseNotFound));
        }

        if (leaseIterator->second.ExpiresAt <= now)
        {
            m_ActiveLeases.erase(leaseIterator);
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseExpired));
        }

        leaseIterator->second.ExpiresAt = now + leaseIterator->second.Lease.Duration;
        return leaseIterator->second.Lease;
    }

    Error::Api::Result<void> Manager::ReleaseLease(const Api::LeaseId& leaseId)
    {
        if (const auto leaseIdValidation = ValidateLeaseId(leaseId); !leaseIdValidation)
        {
            return std::unexpected(leaseIdValidation.error());
        }

        std::scoped_lock lock(m_Mutex);
        const auto leaseIterator = m_ActiveLeases.find(leaseId.Value);
        if (leaseIterator == m_ActiveLeases.end())
        {
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseNotFound));
        }

        m_ActiveLeases.erase(leaseIterator);
        return {};
    }

    Error::Api::Result<Api::LeaseValidation> Manager::ValidateLease(
        const Api::LeaseId& leaseId,
        const Api::ResourceId& resourceId) const
    {
        if (const auto leaseIdValidation = ValidateLeaseId(leaseId); !leaseIdValidation)
        {
            return std::unexpected(leaseIdValidation.error());
        }

        if (const auto resourceValidation = ValidateResourceId(resourceId); !resourceValidation)
        {
            return std::unexpected(resourceValidation.error());
        }

        const auto now = m_Clock();
        std::scoped_lock lock(m_Mutex);
        if (!m_Resources.contains(resourceId.Value))
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::ResourceNotFound));
        }

        const auto leaseIterator = m_ActiveLeases.find(leaseId.Value);
        if (leaseIterator == m_ActiveLeases.end())
        {
            return Api::LeaseValidation{.IsValid = false};
        }

        if (leaseIterator->second.ExpiresAt <= now)
        {
            m_ActiveLeases.erase(leaseIterator);
            return Api::LeaseValidation{.IsValid = false};
        }

        return Api::LeaseValidation{.IsValid = leaseIterator->second.Lease.Resource == resourceId};
    }

    Error::Api::Result<void> Manager::RegisterResource(const Api::ResourceDescriptor& resource)
    {
        if (const auto resourceValidation = ValidateResourceDescriptor(resource); !resourceValidation)
        {
            return std::unexpected(resourceValidation.error());
        }

        std::scoped_lock lock(m_Mutex);
        const auto [_, inserted] = m_Resources.emplace(resource.Id.Value, resource);
        if (!inserted)
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::ResourceAlreadyRegistered));
        }

        return {};
    }

    Error::Api::Result<Api::ResourceDescriptor> Manager::GetResource(const Api::ResourceId& resourceId) const
    {
        if (const auto resourceValidation = ValidateResourceId(resourceId); !resourceValidation)
        {
            return std::unexpected(resourceValidation.error());
        }

        std::scoped_lock lock(m_Mutex);
        const auto resourceIterator = m_Resources.find(resourceId.Value);
        if (resourceIterator == m_Resources.end())
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::ResourceNotFound));
        }

        return resourceIterator->second;
    }

    Error::Api::Result<Api::LeaseId> Manager::GenerateSecureLeaseId()
    {
        std::array<unsigned char, LeaseIdByteCount> randomBytes{};
        if (RAND_bytes(randomBytes.data(), static_cast<int>(randomBytes.size())) != 1)
        {
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseIdGenerationFailed));
        }

        constexpr std::string_view HexDigits = "0123456789abcdef";
        std::string encoded(randomBytes.size() * 2, '\0');
        for (std::size_t index = 0; index < randomBytes.size(); ++index)
        {
            encoded[index * 2] = HexDigits[randomBytes[index] >> 4];
            encoded[index * 2 + 1] = HexDigits[randomBytes[index] & 0x0F];
        }

        return Api::LeaseId{.Value = std::move(encoded)};
    }

    Error::Api::Result<void> Manager::ValidateResourceId(const Api::ResourceId& resourceId)
    {
        if (resourceId.Value.empty())
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::InvalidResourceId));
        }

        return {};
    }

    Error::Api::Result<void> Manager::ValidateLeaseId(const Api::LeaseId& leaseId)
    {
        if (leaseId.Value.empty())
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::InvalidLeaseId));
        }

        return {};
    }

    Error::Api::Result<void> Manager::ValidateResourceDescriptor(const Api::ResourceDescriptor& resource)
    {
        if (const auto resourceValidation = ValidateResourceId(resource.Id); !resourceValidation)
        {
            return std::unexpected(resourceValidation.error());
        }

        if (resource.Policy.LeaseDuration <= std::chrono::milliseconds::zero())
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::InvalidLeaseDuration));
        }

        if (resource.Policy.MaxLeases.has_value() && resource.Policy.MaxLeases.value() == 0)
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::InvalidMaxLeases));
        }

        return {};
    }

    void Manager::PurgeExpiredLeasesLocked(const TimePoint& now) const
    {
        for (auto iterator = m_ActiveLeases.begin(); iterator != m_ActiveLeases.end();)
        {
            if (iterator->second.ExpiresAt <= now)
            {
                iterator = m_ActiveLeases.erase(iterator);
            }
            else
            {
                ++iterator;
            }
        }
    }

    std::size_t Manager::CountActiveLeasesForResourceLocked(const std::string& resourceId) const
    {
        return static_cast<std::size_t>(std::count_if(
            m_ActiveLeases.begin(),
            m_ActiveLeases.end(),
            [&resourceId](const auto& entry) { return entry.second.Lease.Resource.Value == resourceId; }));
    }
}
