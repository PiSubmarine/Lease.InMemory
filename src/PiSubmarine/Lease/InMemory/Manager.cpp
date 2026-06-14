#include "PiSubmarine/Lease/InMemory/Manager.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include "PiSubmarine/Error/Api/MakeError.h"
#include "PiSubmarine/Lease/Api/ErrorCode.h"

namespace PiSubmarine::Lease::InMemory
{
    namespace
    {
        constexpr std::size_t LeaseIdByteCount = 32;
        constexpr std::size_t LeaseSecretByteCount = 32;
        constexpr std::size_t MaxLeaseIdGenerationAttempts = 8;

        [[nodiscard]] Error::Api::Error MakeContractError(const Api::ErrorCode code) noexcept
        {
            return Error::Api::MakeError(Error::Api::ErrorCondition::ContractError, make_error_code(code));
        }

        [[nodiscard]] Error::Api::Error MakeDeviceError(const Api::ErrorCode code) noexcept
        {
            return Error::Api::MakeError(Error::Api::ErrorCondition::DeviceError, make_error_code(code));
        }

        [[nodiscard]] std::shared_ptr<spdlog::logger> CreateLogger(Logging::Api::IFactory& loggerFactory)
        {
            auto logger = loggerFactory.CreateLogger("Lease.InMemory");
            if (!logger)
            {
                throw std::invalid_argument("Lease.InMemory requires a logger factory that returns a logger");
            }

            return logger;
        }
    }

    Manager::Manager(
        Logging::Api::IFactory& loggerFactory,
        Clock clock,
        LeaseIdGenerator leaseIdGenerator,
        LeaseSecretGenerator leaseSecretGenerator)
        : m_Clock(std::move(clock))
        , m_LeaseIdGenerator(std::move(leaseIdGenerator))
        , m_LeaseSecretGenerator(std::move(leaseSecretGenerator))
        , m_Logger(CreateLogger(loggerFactory))
    {
        if (!m_LeaseIdGenerator)
        {
            m_LeaseIdGenerator = [] { return GenerateSecureLeaseId(); };
        }

        if (!m_LeaseSecretGenerator)
        {
            m_LeaseSecretGenerator = [] { return GenerateSecureLeaseSecret(); };
        }

        SPDLOG_LOGGER_INFO(m_Logger, "Initialized in-memory lease manager");
    }

    Error::Api::Result<Api::LeaseGrant> Manager::AcquireLease(const Api::LeaseRequest& request)
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
            SPDLOG_LOGGER_WARN(m_Logger, "Rejected lease request for unknown resource '{}'", request.Resource.Value);
            return std::unexpected(MakeContractError(Api::ErrorCode::ResourceNotFound));
        }

        const auto& resource = resourceIterator->second;
        if (resource.Policy.MaxLeases.has_value() &&
            CountActiveLeasesForResourceLocked(request.Resource.Value) >= resource.Policy.MaxLeases.value())
        {
            SPDLOG_LOGGER_WARN(
                m_Logger,
                "Rejected lease request for resource '{}' because active lease limit {} was reached",
                request.Resource.Value,
                resource.Policy.MaxLeases.value());
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

            const auto leaseSecretResult = m_LeaseSecretGenerator();
            if (!leaseSecretResult)
            {
                return std::unexpected(leaseSecretResult.error());
            }

            if (const auto leaseSecretValidation = ValidateLeaseSecret(*leaseSecretResult); !leaseSecretValidation)
            {
                return std::unexpected(leaseSecretValidation.error());
            }

            if (m_ActiveLeases.contains(leaseIdResult->Value))
            {
                SPDLOG_LOGGER_WARN(m_Logger, "Generated colliding lease id for resource '{}'; retrying", request.Resource.Value);
                continue;
            }

            ActiveLease activeLease{
                .Lease = Api::Lease{
                    .Id = *leaseIdResult,
                    .Resource = request.Resource,
                    .Duration = resource.Policy.LeaseDuration},
                .Secret = *leaseSecretResult,
                .ExpiresAt = now + resource.Policy.LeaseDuration};
            m_ActiveLeases.emplace(activeLease.Lease.Id.Value, activeLease);
            SPDLOG_LOGGER_INFO(
                m_Logger,
                "Issued lease '{}' for resource '{}' with duration {} ms",
                activeLease.Lease.Id.Value,
                activeLease.Lease.Resource.Value,
                activeLease.Lease.Duration.count());
            return Api::LeaseGrant{
                .Lease = activeLease.Lease,
                .Secret = activeLease.Secret};
        }

        SPDLOG_LOGGER_ERROR(m_Logger, "Failed to generate unique lease id for resource '{}'", request.Resource.Value);
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
            SPDLOG_LOGGER_WARN(m_Logger, "Failed to renew unknown lease '{}'", leaseId.Value);
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseNotFound));
        }

        if (leaseIterator->second.ExpiresAt <= now)
        {
            m_ActiveLeases.erase(leaseIterator);
            SPDLOG_LOGGER_WARN(m_Logger, "Failed to renew expired lease '{}'", leaseId.Value);
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseExpired));
        }

        leaseIterator->second.ExpiresAt = now + leaseIterator->second.Lease.Duration;
        SPDLOG_LOGGER_INFO(
            m_Logger,
            "Renewed lease '{}' for resource '{}'",
            leaseIterator->second.Lease.Id.Value,
            leaseIterator->second.Lease.Resource.Value);
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
            SPDLOG_LOGGER_WARN(m_Logger, "Failed to release unknown lease '{}'", leaseId.Value);
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseNotFound));
        }

        SPDLOG_LOGGER_INFO(
            m_Logger,
            "Released lease '{}' for resource '{}'",
            leaseIterator->second.Lease.Id.Value,
            leaseIterator->second.Lease.Resource.Value);
        m_ActiveLeases.erase(leaseIterator);
        return {};
    }

    Error::Api::Result<Api::LeaseSecret> Manager::GetLeaseSecret(const Api::LeaseId& leaseId) const
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
            SPDLOG_LOGGER_WARN(m_Logger, "Failed to get secret for unknown lease '{}'", leaseId.Value);
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseNotFound));
        }

        if (leaseIterator->second.ExpiresAt <= now)
        {
            m_ActiveLeases.erase(leaseIterator);
            SPDLOG_LOGGER_WARN(m_Logger, "Failed to get secret for expired lease '{}'", leaseId.Value);
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseExpired));
        }

        return leaseIterator->second.Secret;
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
            SPDLOG_LOGGER_WARN(
                m_Logger,
                "Lease validation for '{}' failed because resource '{}' is not registered",
                leaseId.Value,
                resourceId.Value);
            return std::unexpected(MakeContractError(Api::ErrorCode::ResourceNotFound));
        }

        const auto leaseIterator = m_ActiveLeases.find(leaseId.Value);
        if (leaseIterator == m_ActiveLeases.end())
        {
            SPDLOG_LOGGER_DEBUG(m_Logger, "Lease '{}' is not active for resource '{}'", leaseId.Value, resourceId.Value);
            return Api::LeaseValidation{.IsValid = false};
        }

        if (leaseIterator->second.ExpiresAt <= now)
        {
            m_ActiveLeases.erase(leaseIterator);
            SPDLOG_LOGGER_DEBUG(m_Logger, "Lease '{}' expired during validation", leaseId.Value);
            return Api::LeaseValidation{.IsValid = false};
        }

        const auto isValid = leaseIterator->second.Lease.Resource == resourceId;
        SPDLOG_LOGGER_DEBUG(
            m_Logger,
            "Validated lease '{}' for resource '{}': {}",
            leaseId.Value,
            resourceId.Value,
            isValid);
        return Api::LeaseValidation{.IsValid = isValid};
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
            SPDLOG_LOGGER_WARN(m_Logger, "Rejected duplicate resource registration '{}'", resource.Id.Value);
            return std::unexpected(MakeContractError(Api::ErrorCode::ResourceAlreadyRegistered));
        }

        SPDLOG_LOGGER_INFO(
            m_Logger,
            "Registered resource '{}' with lease duration {} ms and max leases {}",
            resource.Id.Value,
            resource.Policy.LeaseDuration.count(),
            resource.Policy.MaxLeases.has_value() ? std::to_string(resource.Policy.MaxLeases.value()) : std::string("unlimited"));
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
            SPDLOG_LOGGER_WARN(m_Logger, "Failed to get unknown resource '{}'", resourceId.Value);
            return std::unexpected(MakeContractError(Api::ErrorCode::ResourceNotFound));
        }

        SPDLOG_LOGGER_DEBUG(m_Logger, "Returned descriptor for resource '{}'", resourceId.Value);
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

    Error::Api::Result<Api::LeaseSecret> Manager::GenerateSecureLeaseSecret()
    {
        std::vector<std::byte> secret(LeaseSecretByteCount);
        if (RAND_bytes(reinterpret_cast<unsigned char*>(secret.data()), static_cast<int>(secret.size())) != 1)
        {
            return std::unexpected(MakeDeviceError(Api::ErrorCode::LeaseSecretGenerationFailed));
        }

        return Api::LeaseSecret{.Value = std::move(secret)};
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

    Error::Api::Result<void> Manager::ValidateLeaseSecret(const Api::LeaseSecret& leaseSecret)
    {
        if (leaseSecret.Value.empty())
        {
            return std::unexpected(MakeContractError(Api::ErrorCode::InvalidLeaseSecret));
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
                SPDLOG_LOGGER_DEBUG(
                    m_Logger,
                    "Purged expired lease '{}' for resource '{}'",
                    iterator->second.Lease.Id.Value,
                    iterator->second.Lease.Resource.Value);
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
