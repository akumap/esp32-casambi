/**
 * Semantic layer over the INVOCATION frame stream (packet_parse.h's
 * `InvocationFrame`, from an incoming 0x07 payload). Deliberately kept
 * separate from the pure framing parser: framing determines frame
 * boundaries and decodes the header, this layer decides what a frame MEANS
 * (button press/release, physical-input notification, or "not recognized").
 *
 * Arduino-free (only the C++ standard library) so it is host-testable, same
 * rationale as packet_parse.h.
 *
 * UNVERIFIED — like the framing it builds on, none of this has been checked
 * against a real capture (0x07 has never been observed on the reference
 * network; see packet_parse.h's parseInvocationStream doc comment and
 * docs/casambi-protokoll-referenz.md B.12a). Classification here never feeds
 * `_applyUnitStates` — it only ever produces diagnostic/callback output.
 */

#ifndef INVOCATION_EVENTS_H
#define INVOCATION_EVENTS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "packet_parse.h"

namespace invocation_events {

// ============================================================================
// Classification
// ============================================================================

enum class InputEventType : uint8_t {
    RawInput,               // NotifyInput frame with an unmapped code
    ButtonPress,
    ButtonRelease,
    ButtonHold,
    ButtonReleaseAfterHold,
};

// Only buttonIndex/inputIndex 0-3 have an observed physical label; indices
// 4-7 exist on the wire (the opcode ranges allow up to 8) but no mapping has
// ever been observed for them, so `label` stays 0 there — never guessed at.
constexpr uint8_t BUTTON_LABELS[4] = {4, 1, 2, 3};

struct CasambiInputEvent {
    InputEventType type = InputEventType::RawInput;

    uint8_t unitId = 0;    // frame.targetId() — the unit that reported the input
    uint8_t index = 0;     // buttonIndex (0-7) or inputIndex (0-7)
    uint8_t label = 0;     // BUTTON_LABELS[index] for index<4 on a button frame, else 0

    uint8_t inputCode = 0; // NotifyInput raw code (0 for button-stream events)
    uint8_t channel = 0;   // NotifyInput channel (payload[1] & 0x07)

    bool     hasValue16 = false;
    uint16_t value16 = 0;  // little-endian, only when payloadLen >= 4

    bool    pressed = false;   // button-stream events only
    uint8_t p = 0;             // button-stream payload[0] bits 3-6
    uint8_t s = 0;             // button-stream payload[0] bits 0-2

    uint16_t origin = 0;
    uint16_t age = 0;
    bool     hasOriginHandle = false;
    uint8_t  originHandle = 0;

    bool isButtonStream = false;   // true for ButtonPress/Release from the button stream
};

inline InputEventType mapInputCode(uint8_t code) {
    switch (code) {
        case 0x01: return InputEventType::ButtonPress;
        case 0x02: return InputEventType::ButtonRelease;
        case 0x09: return InputEventType::ButtonHold;
        case 0x0C: return InputEventType::ButtonReleaseAfterHold;
        default:   return InputEventType::RawInput;
    }
}

// Fills common origin/age/originHandle fields shared by both frame kinds.
inline void fillCommon(const InvocationFrame& frame, CasambiInputEvent& ev) {
    ev.unitId = frame.targetId();
    ev.origin = frame.origin;
    ev.age = frame.age;
    ev.hasOriginHandle = frame.hasOriginHandle;
    ev.originHandle = frame.originHandle;
}

// Button stream: targetType 0x06, opcode 29-36 (ButtonEvent0..7).
inline bool classifyButtonFrame(const InvocationFrame& frame, CasambiInputEvent& ev) {
    if (frame.targetType() != 0x06) return false;
    if (frame.opcode < 29 || frame.opcode > 36) return false;
    if (frame.payloadLen < 1) return false;

    uint8_t buttonIndex = static_cast<uint8_t>(frame.opcode - 29);
    uint8_t b0 = frame.payload[0];
    bool pressed = (b0 & 0x80) != 0;

    fillCommon(frame, ev);
    ev.index = buttonIndex;
    ev.label = (buttonIndex < 4) ? BUTTON_LABELS[buttonIndex] : 0;
    ev.pressed = pressed;
    ev.p = (b0 >> 3) & 0x0F;
    ev.s = b0 & 0x07;
    ev.type = pressed ? InputEventType::ButtonPress : InputEventType::ButtonRelease;
    ev.isButtonStream = true;
    return true;
}

// NotifyInput stream: targetType 0x12, opcode 64-71 (NotifyInput0..7).
inline bool classifyNotifyInputFrame(const InvocationFrame& frame, CasambiInputEvent& ev) {
    if (frame.targetType() != 0x12) return false;
    if (frame.opcode < 64 || frame.opcode > 71) return false;
    if (frame.payloadLen < 2) return false;

    uint8_t inputIndex = static_cast<uint8_t>(frame.opcode - 64);
    uint8_t inputCode = frame.payload[0];
    uint8_t inputB1 = frame.payload[1];

    fillCommon(frame, ev);
    ev.index = inputIndex;
    ev.inputCode = inputCode;
    ev.channel = inputB1 & 0x07;
    ev.type = mapInputCode(inputCode);
    ev.isButtonStream = false;

    if (frame.payloadLen >= 4) {
        ev.hasValue16 = true;
        ev.value16 = static_cast<uint16_t>(frame.payload[2]) |
                     (static_cast<uint16_t>(frame.payload[3]) << 8);
    }
    return true;
}

// Tries the button stream first, then NotifyInput — the two target types are
// disjoint (0x06 vs 0x12) so there is no ambiguity either way. Returns false
// for a frame that matches neither shape; that is NOT a parse error (see
// packet_parse.h's tolerant-but-honest contract) — such frames are simply
// not classified here and are left to the raw per-frame callback.
inline bool classifyInvocationEvent(const InvocationFrame& frame, CasambiInputEvent& ev) {
    if (classifyButtonFrame(frame, ev)) return true;
    if (classifyNotifyInputFrame(frame, ev)) return true;
    return false;
}

// ============================================================================
// Deduplication
// ============================================================================

struct EventStateEntry {
    bool used = false;

    uint8_t unitId = 0;
    uint8_t index = 0;

    int8_t  lastButtonState = -1;  // -1 unknown, 0 released, 1 pressed
    int16_t lastInputCode = -1;    // -1 unknown

    bool buttonStreamSeen = false;
};

// Small fixed-size table, keyed by (unitId, index). Deliberately fixed-size
// for an embedded target rather than a map. If more than CAPACITY distinct
// (unitId, index) pairs are active at once, new pairs beyond the capacity
// fail OPEN (no dedup state available, so events for them pass through
// unfiltered) rather than dropping events or evicting an existing entry.
struct EventDedupTable {
    static constexpr size_t CAPACITY = 32;
    std::array<EventStateEntry, CAPACITY> entries{};
};

namespace detail {

// Finds the entry for (unitId, index), allocating an unused slot if needed.
// Returns nullptr if none exists and the table is full (fail-open case).
inline EventStateEntry* findOrAlloc(EventDedupTable& table, uint8_t unitId, uint8_t index) {
    EventStateEntry* freeSlot = nullptr;
    for (auto& e : table.entries) {
        if (e.used && e.unitId == unitId && e.index == index) return &e;
        if (!e.used && freeSlot == nullptr) freeSlot = &e;
    }
    if (freeSlot) {
        freeSlot->used = true;
        freeSlot->unitId = unitId;
        freeSlot->index = index;
        return freeSlot;
    }
    return nullptr;
}

inline bool shouldEmitButtonEdge(EventStateEntry& state, bool pressed) {
    int8_t next = pressed ? 1 : 0;
    if (state.lastButtonState == next) return false;
    state.lastButtonState = next;
    return true;
}

inline bool shouldEmitInputCode(EventStateEntry& state, uint8_t inputCode) {
    if (state.lastInputCode == inputCode) return false;
    state.lastInputCode = inputCode;
    return true;
}

}  // namespace detail

// Classifies every frame in `frames` into `events`, then applies dedup:
//  - repeated button-stream press/release edges for the same (unitId, index)
//    are suppressed, only the transition is emitted;
//  - repeated identical NotifyInput codes for the same (unitId, index) are
//    suppressed;
//  - a button-stream frame anywhere in THIS packet suppresses a matching
//    NotifyInput Press(0x01)/Release(0x02) for the same (unitId, index) in
//    the same packet — Hold/ReleaseAfterHold are still emitted regardless
//    (packet-local, order-independent: all frames are classified first, then
//    button-stream units are marked, then NotifyInput press/release for
//    those units is suppressed).
// Frames that classify as neither button nor NotifyInput are left out of
// `events` entirely (see classifyInvocationEvent) — callers get full-fidelity
// access to every frame, classified or not, via the raw per-frame callback.
inline void decodeInvocationEvents(const std::vector<InvocationFrame>& frames,
                                   EventDedupTable& table,
                                   std::vector<CasambiInputEvent>& events) {
    events.clear();

    std::vector<CasambiInputEvent> classified;
    classified.reserve(frames.size());
    for (const auto& frame : frames) {
        CasambiInputEvent ev;
        if (classifyInvocationEvent(frame, ev)) classified.push_back(ev);
    }

    // `buttonStreamSeen` reflects only THIS packet, not history — reset every
    // call before (re-)marking it below, otherwise a button-stream frame seen
    // once would suppress NotifyInput press/release forever afterwards.
    for (auto& e : table.entries) e.buttonStreamSeen = false;

    // Mark (unitId, index) pairs that had a button-stream frame in this packet.
    for (const auto& ev : classified) {
        if (!ev.isButtonStream) continue;
        EventStateEntry* state = detail::findOrAlloc(table, ev.unitId, ev.index);
        if (state) state->buttonStreamSeen = true;
    }

    for (const auto& ev : classified) {
        EventStateEntry* state = detail::findOrAlloc(table, ev.unitId, ev.index);
        if (!state) {
            // Table full: fail open, no dedup possible for this pair.
            events.push_back(ev);
            continue;
        }

        if (ev.isButtonStream) {
            if (detail::shouldEmitButtonEdge(*state, ev.pressed)) events.push_back(ev);
            continue;
        }

        bool suppressAsRedundantWithButtonStream =
            state->buttonStreamSeen &&
            (ev.type == InputEventType::ButtonPress || ev.type == InputEventType::ButtonRelease);
        if (suppressAsRedundantWithButtonStream) continue;

        if (detail::shouldEmitInputCode(*state, ev.inputCode)) events.push_back(ev);
    }
}

}  // namespace invocation_events

#endif  // INVOCATION_EVENTS_H
