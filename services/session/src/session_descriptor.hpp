#ifndef LAPIS_SESSION_DESCRIPTOR_HPP
#define LAPIS_SESSION_DESCRIPTOR_HPP
#include "transport/local_protocol.hpp"
#include <memory>
#include <optional>
namespace lapis::session {
// Bounded owner-only hint at endpoint + ".session". Not proof of liveness.
// Absence returns nullopt; malformed, unsafe or mismatched files throw.
[[nodiscard]] std::optional<wire::SessionIdentity> read_descriptor(const QString& endpoint,
                                                                   const QByteArray& fingerprint);
// Atomic 0600 replacement in an already validated private endpoint parent.
// Refuse links/nonregular/foreign or insecure existing files, even during discovery.
void write_descriptor(const QString& endpoint, const QByteArray& fingerprint,
                      const wire::SessionIdentity& identity);

// A staged descriptor becomes durable off the caller thread. Only the newest
// ticket for a canonical endpoint may rename; cancelling a ticket is ordered
// with that short final commit guard.
class DescriptorTicket final {
  public:
    ~DescriptorTicket();
    DescriptorTicket(const DescriptorTicket&) = delete;
    DescriptorTicket& operator=(const DescriptorTicket&) = delete;

    // One worker calls stage then commit; cancel may run from the owning UI thread.
    // Create and fsync the private temporary file. Must run off the GUI thread.
    void stage();
    // Returns an empty string after replacement, otherwise a display message.
    [[nodiscard]] QString commit();
    void cancel();

  private:
    friend class DescriptorStore;
    struct State;
    explicit DescriptorTicket(std::shared_ptr<State> state);
    std::shared_ptr<State> state_;
};

class DescriptorStore {
  public:
    DescriptorStore() = default;
    virtual ~DescriptorStore() = default;
    DescriptorStore(const DescriptorStore&) = delete;
    DescriptorStore& operator=(const DescriptorStore&) = delete;

    [[nodiscard]] std::shared_ptr<DescriptorTicket> prepare(const QString& endpoint,
                                                            const QByteArray& fingerprint,
                                                            const wire::SessionIdentity& identity);
    [[nodiscard]] QString commit(DescriptorTicket& ticket);
    void write(const QString& endpoint, const QByteArray& fingerprint,
               const wire::SessionIdentity& identity);

  protected:
    // The final endpoint check and rename happen only after this returns.
    virtual void before_commit(const DescriptorTicket&);
};
} // namespace lapis::session
#endif
