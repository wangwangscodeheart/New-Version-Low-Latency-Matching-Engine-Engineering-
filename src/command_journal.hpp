#ifndef COMMAND_JOURNAL_HPP
#define COMMAND_JOURNAL_HPP

#include "submission_gate.hpp"
#include "system_events.hpp"
#include <cstddef>

// Single-writer, non-owning integration point for recovery-critical journals.
// begin reserves a complete command's record budget BEFORE external publication
// or book mutation. commit accepts the whole group, not individual records.
// Acceptance is in-process only; it is not a durability acknowledgment.
class CommandJournal : public SubmissionGate {
public:
    virtual bool begin_command(Timestamp timestamp, const Symbol& symbol,
                               const Command& command, size_t max_records) = 0;
    virtual bool append_trade(const TradeEvent& trade) = 0;
    virtual bool commit_command(CommandOutcome outcome) = 0;
    // Release only an uncommitted group. Must not roll back an accepted group.
    virtual void abort_command() noexcept = 0;
};

#endif
