# Lease.InMemory Architecture

`PiSubmarine.Lease.InMemory` provides an in-memory implementation of the `PiSubmarine.Lease.Api` contracts.

## Scope

- resource registration
- lease issuance
- lease renewal
- lease release
- lease validation

## Security

Lease ids are opaque bearer tokens. The default implementation generates 256-bit lease ids using OpenSSL secure random
bytes and encodes them as lowercase hexadecimal strings.

## Concurrency

`PiSubmarine::Lease::InMemory::Manager` is thread-safe. Public operations serialize access to the internal state with a
mutex.

## Testing

The manager supports clock and lease-id generator injection so tests can control time progression and collision scenarios
without weakening the default production behavior.
