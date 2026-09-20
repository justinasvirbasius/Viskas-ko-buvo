#include "ein/full_path_executive.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ein {

namespace {

std::size_t validated_event_capacity(std::size_t capacity) {
    if (capacity == 0 || capacity > maximum_executive_event_capacity)
        throw std::invalid_argument(
            "executive event capacity must be within [1, 4096]");
    return capacity;
}

ExecutiveDecision continuing(std::uint64_t frame,
                             ExecutiveStage stage = ExecutiveStage::Idle,
                             std::string reason = {}) {
    return {ExecutiveAction::Continue, ExecutiveFault::None, stage, frame,
            std::move(reason)};
}

} // namespace

FullPathExecutive::FullPathExecutive(FullPathRelay& relay,
                                     ExecutiveBudget budget)
    : relay_(relay), budget_(budget),
      event_ring_(validated_event_capacity(budget.event_capacity)) {
    if (budget_.frame_microseconds == 0)
        throw std::invalid_argument("executive frame budget must be non-zero");
    if (budget_.no_progress_microseconds == 0)
        throw std::invalid_argument(
            "executive no-progress budget must be non-zero");
    if (budget_.lan_return_reserve_microseconds >=
        budget_.frame_microseconds)
        throw std::invalid_argument(
            "LAN return reserve must be smaller than the frame budget");
    if (std::any_of(budget_.stage_microseconds.begin(),
                    budget_.stage_microseconds.end(),
                    [](std::uint64_t value) { return value == 0; }))
        throw std::invalid_argument(
            "every executive stage budget must be non-zero");
    if (budget_.failures_before_cooldown == 0)
        throw std::invalid_argument(
            "executive failure threshold must be non-zero");
}

std::size_t FullPathExecutive::stage_index(ExecutiveStage stage) {
    const auto value = static_cast<std::size_t>(stage);
    if (value == 0 || value > executive_stage_count)
        throw std::invalid_argument("idle or unknown executive stage has no budget");
    return value - 1u;
}

std::uint64_t FullPathExecutive::deadline_after(
    std::uint64_t start, std::uint64_t duration) noexcept {
    if (duration > std::numeric_limits<std::uint64_t>::max() - start)
        return std::numeric_limits<std::uint64_t>::max();
    return start + duration;
}

std::string FullPathExecutive::bounded_reason(std::string_view reason) {
    if (reason.empty()) return "executive recovery requested";
    return std::string(reason.substr(0, maximum_executive_reason_bytes));
}

void FullPathExecutive::publish_event(ExecutiveEvent event) noexcept {
    if (event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        if (dropped_events_ != std::numeric_limits<std::uint64_t>::max())
            ++dropped_events_;
        return;
    }
    if (event.reason.size() > maximum_executive_reason_bytes)
        event.reason.resize(maximum_executive_reason_bytes);

    const bool overwrite = event_size_ == event_ring_.size();
    const auto index = overwrite
                           ? event_head_
                           : (event_head_ + event_size_) % event_ring_.size();
    event.sequence = event_sequence_ + 1u;
    try {
        event_ring_[index] = std::move(event);
    } catch (...) {
        if (dropped_events_ != std::numeric_limits<std::uint64_t>::max())
            ++dropped_events_;
        return;
    }
    ++event_sequence_;
    if (overwrite) {
        event_head_ = (event_head_ + 1u) % event_ring_.size();
        if (dropped_events_ != std::numeric_limits<std::uint64_t>::max())
            ++dropped_events_;
    } else {
        ++event_size_;
    }
}

PathFramePlan FullPathExecutive::begin_frame(
    std::uint64_t now_microseconds, bool camera_cut,
    bool topology_changed) {
    if (frame_active_) {
        (void)recover(stage_ == ExecutiveStage::Idle
                          ? ExecutiveStage::FrameSetup
                          : stage_,
                      ExecutiveFault::AbandonedFrame, now_microseconds,
                      "unfinished frame was replaced without waiting");
    }
    if (frame_ == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("executive frame sequence space exhausted");

    if (cooldown_frames_remaining_ != 0)
        relay_.invalidate("executive recovery cooldown requires FULL");

    auto next_plan = relay_.plan(camera_cut, topology_changed);
    ++frame_;
    frame_active_ = true;
    frame_started_ = now_microseconds;
    frame_deadline_ = deadline_after(now_microseconds,
                                     budget_.frame_microseconds);
    stage_ = ExecutiveStage::Idle;
    stage_started_ = 0;
    stage_deadline_ = 0;
    last_progress_ = now_microseconds;
    progress_token_ = 0;
    plan_ = next_plan;

    publish_event({
        0,
        frame_,
        now_microseconds,
        next_plan.seed_generation,
        0,
        ExecutiveStage::FrameSetup,
        ExecutiveEventKind::FrameStarted,
        ExecutiveAction::Continue,
        ExecutiveFault::None,
        next_plan.reason,
    });
    return next_plan;
}

ExecutiveDecision FullPathExecutive::recover(
    ExecutiveStage stage, ExecutiveFault fault,
    std::uint64_t now_microseconds, std::string reason) {
    reason = bounded_reason(reason);

    ExecutiveAction action = ExecutiveAction::ForceFull;
    const bool peripheral_fault =
        fault == ExecutiveFault::StageDeadline ||
        fault == ExecutiveFault::NoProgress ||
        fault == ExecutiveFault::Exception;
    if (frame_active_ && peripheral_fault && stage == ExecutiveStage::Lan)
        action = ExecutiveAction::FallbackLocal;
    else if (frame_active_ && peripheral_fault &&
             stage == ExecutiveStage::MemoryRelayer)
        action = ExecutiveAction::RetainSource;

    const auto seed_generation = plan_ ? plan_->seed_generation : 0u;
    const auto recovery_progress = progress_token_;
    stage_ = ExecutiveStage::Idle;
    stage_started_ = 0;
    stage_deadline_ = 0;
    last_progress_ = now_microseconds;
    progress_token_ = 0;

    if (action == ExecutiveAction::ForceFull) {
        relay_.invalidate(reason);
        frame_active_ = false;
        plan_.reset();
        if (fault != ExecutiveFault::ProofRejected) {
            if (consecutive_hard_failures_ !=
                std::numeric_limits<std::uint32_t>::max())
                ++consecutive_hard_failures_;
            if (consecutive_hard_failures_ >=
                budget_.failures_before_cooldown)
                cooldown_frames_remaining_ = std::max(
                    cooldown_frames_remaining_, budget_.full_cooldown_frames);
        }
    }

    // Telemetry is published only after the recovery state is authoritative;
    // an allocation failure while preparing an event cannot preserve a bad
    // candidate or an active wait.
    publish_event({
        0,
        frame_,
        now_microseconds,
        seed_generation,
        recovery_progress,
        stage,
        ExecutiveEventKind::Recovered,
        action,
        fault,
        reason,
    });

    return {action, fault, stage, frame_, std::move(reason)};
}

ExecutiveDecision FullPathExecutive::begin_stage(
    ExecutiveStage stage, std::uint64_t now_microseconds) {
    if (!frame_active_)
        return recover(stage, ExecutiveFault::InvalidSequence,
                       now_microseconds,
                       "cannot begin a stage without an active frame");
    if (stage == ExecutiveStage::Idle ||
        static_cast<std::size_t>(stage) > executive_stage_count)
        return recover(stage, ExecutiveFault::InvalidSequence,
                       now_microseconds,
                       "idle or unknown stage cannot be launched");

    auto decision = poll(now_microseconds);
    if (decision.action != ExecutiveAction::Continue) return decision;
    if (stage_ != ExecutiveStage::Idle)
        return recover(stage_, ExecutiveFault::InvalidSequence,
                       now_microseconds,
                       "a second stage began before the active stage completed");

    stage_ = stage;
    stage_started_ = now_microseconds;
    stage_deadline_ = std::min(
        frame_deadline_,
        deadline_after(now_microseconds,
                       budget_.stage_microseconds[stage_index(stage)]));
    last_progress_ = now_microseconds;
    progress_token_ = 0;
    publish_event({
        0,
        frame_,
        now_microseconds,
        plan_ ? plan_->seed_generation : 0u,
        0,
        stage,
        ExecutiveEventKind::StageStarted,
        ExecutiveAction::Continue,
        ExecutiveFault::None,
        "stage launched without blocking the frame thread",
    });
    return continuing(frame_, stage);
}

ExecutiveDecision FullPathExecutive::heartbeat(
    std::uint64_t progress_token, std::uint64_t now_microseconds) {
    if (!frame_active_ || stage_ == ExecutiveStage::Idle)
        return recover(stage_, ExecutiveFault::InvalidSequence,
                       now_microseconds,
                       "progress arrived without an active stage");

    auto decision = poll(now_microseconds);
    if (decision.action != ExecutiveAction::Continue) return decision;
    if (progress_token != progress_token_) {
        progress_token_ = progress_token;
        last_progress_ = now_microseconds;
        publish_event({
            0,
            frame_,
            now_microseconds,
            plan_ ? plan_->seed_generation : 0u,
            progress_token_,
            stage_,
            ExecutiveEventKind::Progress,
            ExecutiveAction::Continue,
            ExecutiveFault::None,
            "stage progress advanced",
        });
    }
    return continuing(frame_, stage_);
}

ExecutiveDecision FullPathExecutive::poll(
    std::uint64_t now_microseconds) {
    if (!frame_active_) return continuing(frame_);
    if (now_microseconds < frame_started_ ||
        (stage_ != ExecutiveStage::Idle &&
         now_microseconds < stage_started_))
        return recover(stage_, ExecutiveFault::InvalidSequence,
                       now_microseconds,
                       "executive timestamps must be monotonic within a frame");
    if (now_microseconds >= frame_deadline_)
        return recover(stage_ == ExecutiveStage::Idle
                           ? ExecutiveStage::FrameSetup
                           : stage_,
                       ExecutiveFault::FrameDeadline, now_microseconds,
                       "frame deadline expired; unproven output was rejected");
    if (stage_ == ExecutiveStage::Idle) return continuing(frame_);
    if (now_microseconds >= stage_deadline_)
        return recover(stage_, ExecutiveFault::StageDeadline,
                       now_microseconds,
                       "stage deadline expired without delaying presentation");
    if (now_microseconds >= deadline_after(
            last_progress_, budget_.no_progress_microseconds))
        return recover(stage_, ExecutiveFault::NoProgress,
                       now_microseconds,
                       "progress token stopped advancing");
    return continuing(frame_, stage_);
}

ExecutiveDecision FullPathExecutive::complete_stage(
    ExecutiveStage stage, std::uint64_t now_microseconds) {
    if (!frame_active_ || stage_ != stage || stage == ExecutiveStage::Idle)
        return recover(stage_, ExecutiveFault::InvalidSequence,
                       now_microseconds,
                       "stage completion did not match the active stage");
    auto decision = poll(now_microseconds);
    if (decision.action != ExecutiveAction::Continue) return decision;

    const auto completed_stage = stage_;
    const auto completed_progress = progress_token_;
    const auto seed_generation = plan_ ? plan_->seed_generation : 0u;
    stage_ = ExecutiveStage::Idle;
    stage_started_ = 0;
    stage_deadline_ = 0;
    last_progress_ = now_microseconds;
    progress_token_ = 0;
    publish_event({
        0,
        frame_,
        now_microseconds,
        seed_generation,
        completed_progress,
        completed_stage,
        ExecutiveEventKind::StageCompleted,
        ExecutiveAction::Continue,
        ExecutiveFault::None,
        "stage completed inside its deadline",
    });
    return continuing(frame_);
}

ExecutiveDecision FullPathExecutive::report_exception(
    ExecutiveStage stage, std::uint64_t now_microseconds,
    std::string_view detail) {
    if (!frame_active_ || stage_ != stage || stage == ExecutiveStage::Idle)
        return recover(stage_, ExecutiveFault::InvalidSequence,
                       now_microseconds,
                       "exception report did not match the active stage");
    std::string reason(to_string(stage));
    reason += " stage exception: ";
    reason += detail;
    return recover(stage, ExecutiveFault::Exception, now_microseconds,
                   std::move(reason));
}

ExecutiveDecision FullPathExecutive::commit_frame(
    const PathProof& proof, std::uint64_t now_microseconds) {
    if (!frame_active_ || !plan_)
        return recover(ExecutiveStage::Proof,
                       ExecutiveFault::InvalidSequence, now_microseconds,
                       "cannot commit without an active frame plan");
    if (stage_ != ExecutiveStage::Idle)
        return recover(stage_, ExecutiveFault::InvalidSequence,
                       now_microseconds,
                       "cannot commit while a stage is still active");
    auto decision = poll(now_microseconds);
    if (decision.action != ExecutiveAction::Continue) return decision;

    bool accepted = false;
    try {
        accepted = relay_.commit(*plan_, proof);
    } catch (const std::exception& error) {
        std::string reason("proof commit exception: ");
        reason += error.what();
        return recover(ExecutiveStage::Proof, ExecutiveFault::Exception,
                       now_microseconds, std::move(reason));
    } catch (...) {
        return recover(ExecutiveStage::Proof, ExecutiveFault::Exception,
                       now_microseconds,
                       "proof commit raised a non-standard exception");
    }
    if (!accepted)
        return recover(ExecutiveStage::Proof,
                       ExecutiveFault::ProofRejected, now_microseconds,
                       "correctness proof rejected the candidate; FULL required");

    const auto committed_seed_generation = plan_->seed_generation;
    frame_active_ = false;
    stage_ = ExecutiveStage::Idle;
    plan_.reset();
    consecutive_hard_failures_ = 0;
    if (cooldown_frames_remaining_ != 0) --cooldown_frames_remaining_;
    publish_event({
        0,
        frame_,
        now_microseconds,
        committed_seed_generation,
        0,
        ExecutiveStage::Proof,
        ExecutiveEventKind::FrameCommitted,
        ExecutiveAction::Continue,
        ExecutiveFault::None,
        "verified frame committed to history",
    });
    return continuing(frame_, ExecutiveStage::Proof,
                      "verified frame committed to history");
}

bool FullPathExecutive::should_dispatch_lan(
    std::uint64_t now_microseconds,
    std::uint64_t expected_round_trip_microseconds) const noexcept {
    if (!frame_active_ || !plan_ || stage_ != ExecutiveStage::Idle ||
        plan_->mode == PathFrameMode::FullSeed ||
        now_microseconds < frame_started_ ||
        now_microseconds >= frame_deadline_)
        return false;
    if (expected_round_trip_microseconds >
        budget_.stage_microseconds[
            static_cast<std::size_t>(ExecutiveStage::Lan) - 1u])
        return false;
    const auto remaining = frame_deadline_ - now_microseconds;
    if (expected_round_trip_microseconds > remaining) return false;
    return budget_.lan_return_reserve_microseconds <=
           remaining - expected_round_trip_microseconds;
}

ExecutiveEventBatch FullPathExecutive::events_after(
    std::uint64_t cursor, std::size_t limit) const {
    if (limit == 0)
        throw std::invalid_argument("executive event limit must be non-zero");
    if (cursor > event_sequence_)
        throw std::invalid_argument(
            "executive event cursor is from the future");

    ExecutiveEventBatch batch;
    batch.next_cursor = cursor;
    batch.dropped_total = dropped_events_;
    if (event_size_ == 0) return batch;

    const auto& oldest = *event_ring_[event_head_];
    if (cursor < oldest.sequence - 1u)
        batch.lost_before_batch = oldest.sequence - cursor - 1u;

    const auto bounded_limit = std::min<std::size_t>(limit, 1024);
    batch.events.reserve(std::min(bounded_limit, event_size_));
    for (std::size_t offset = 0; offset < event_size_; ++offset) {
        const auto index = (event_head_ + offset) % event_ring_.size();
        const auto& event = *event_ring_[index];
        if (event.sequence <= cursor) continue;
        batch.events.push_back(event);
        batch.next_cursor = event.sequence;
        if (batch.events.size() == bounded_limit) break;
    }
    return batch;
}

std::string_view to_string(ExecutiveStage value) noexcept {
    switch (value) {
        case ExecutiveStage::Idle: return "idle";
        case ExecutiveStage::FrameSetup: return "frame-setup";
        case ExecutiveStage::Sense: return "sense";
        case ExecutiveStage::Classify: return "classify";
        case ExecutiveStage::Lan: return "lan";
        case ExecutiveStage::NativeRefine: return "native-refine";
        case ExecutiveStage::Resolve: return "resolve";
        case ExecutiveStage::Proof: return "proof";
        case ExecutiveStage::MemoryRelayer: return "memory-relayer";
        case ExecutiveStage::Present: return "present";
    }
    return "unknown";
}

std::string_view to_string(ExecutiveAction value) noexcept {
    switch (value) {
        case ExecutiveAction::Continue: return "continue";
        case ExecutiveAction::FallbackLocal: return "fallback-local";
        case ExecutiveAction::RetainSource: return "retain-source";
        case ExecutiveAction::ForceFull: return "force-full";
    }
    return "unknown";
}

std::string_view to_string(ExecutiveFault value) noexcept {
    switch (value) {
        case ExecutiveFault::None: return "none";
        case ExecutiveFault::FrameDeadline: return "frame-deadline";
        case ExecutiveFault::StageDeadline: return "stage-deadline";
        case ExecutiveFault::NoProgress: return "no-progress";
        case ExecutiveFault::Exception: return "exception";
        case ExecutiveFault::InvalidSequence: return "invalid-sequence";
        case ExecutiveFault::AbandonedFrame: return "abandoned-frame";
        case ExecutiveFault::ProofRejected: return "proof-rejected";
    }
    return "unknown";
}

std::string_view to_string(ExecutiveEventKind value) noexcept {
    switch (value) {
        case ExecutiveEventKind::FrameStarted: return "frame-started";
        case ExecutiveEventKind::StageStarted: return "stage-started";
        case ExecutiveEventKind::Progress: return "progress";
        case ExecutiveEventKind::StageCompleted: return "stage-completed";
        case ExecutiveEventKind::Recovered: return "recovered";
        case ExecutiveEventKind::FrameCommitted: return "frame-committed";
    }
    return "unknown";
}

} // namespace ein
