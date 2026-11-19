#ifndef COMMITTED_COMMAND_SINK_HPP
#define COMMITTED_COMMAND_SINK_HPP

#include "events.hpp"
#include "instrument_id.hpp"
#include "system_events.hpp"
#include <vector>

// Single-owner, in-process handoff AFTER CommandJournal accepts the complete
// command. Acceptance is not fsync/durability. Failure cannot undo matching.
class CommittedCommandSink {
public:
    virtual ~CommittedCommandSink() = default;
    virtual bool available() const noexcept = 0;
    virtual bool publish(Timestamp timestamp, InstrumentId instrument,
                         const Command& command, CommandOutcome outcome,
                         const std::vector<EngineEvent>& events) = 0;
    virtual void invalidate() noexcept = 0;
};

#endif
