#ifndef SUBMISSION_GATE_HPP
#define SUBMISSION_GATE_HPP

// Optional application-layer availability gate. Recovery-critical components
// implement this interface so TradingEngine can fail-stop before publishing or
// applying the next command.
class SubmissionGate {
public:
    virtual ~SubmissionGate() = default;
    virtual bool available() const noexcept = 0;
};

#endif
