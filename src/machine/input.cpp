// the keyboard and the game port from outside the machine: keys pressed and released, the
// paddles and buttons set, and scripted input queued by cycle (ServiceEvents in apple2.cpp
// applies the queues)

#include "machine/apple2.hpp"

namespace tisple {

void PressKey(Apple2& a, uint8_t code) {
    a.keys.latch = static_cast<uint8_t>(code | 0x80);
    a.keys.key_down = true;
    a.keys.release_cycle = UINT64_MAX;
}

void ReleaseKey(Apple2& a) {
    a.keys.key_down = false;
    a.keys.release_cycle = UINT64_MAX;
}

bool ScheduleKey(Apple2& a, uint64_t cycle, uint8_t code) {
    Keyboard& k = a.keys;
    if (k.queue_count == kMaxScheduledKeys) {
        return false;
    }
    // keep the queue sorted by cycle: insert from the back
    uint32_t i = k.queue_count;
    while (i > 0) {
        const ScheduledKey& prev = k.queue[(k.queue_head + i - 1) % kMaxScheduledKeys];
        if (prev.cycle <= cycle) {
            break;
        }
        k.queue[(k.queue_head + i) % kMaxScheduledKeys] = prev;
        i--;
    }
    k.queue[(k.queue_head + i) % kMaxScheduledKeys] = {cycle, code};
    k.queue_count++;
    return true;
}

void SetPaddle(Apple2& a, int index, uint8_t value) {
    if (index >= 0 && index < 4) {
        a.game.paddle[index] = value;
    }
}

void SetButton(Apple2& a, int index, bool down) {
    if (index >= 0 && index < 3) {
        a.game.button[index] = down;
    }
}

bool ScheduleJoystick(Apple2& a, const ScheduledJoystick& input) {
    GamePort& g = a.game;
    if (g.queue_count == kMaxScheduledJoystick) {
        return false;
    }
    uint32_t i = g.queue_count;
    while (i > 0) {
        const ScheduledJoystick& prev = g.queue[(g.queue_head + i - 1) % kMaxScheduledJoystick];
        if (prev.cycle <= input.cycle) {
            break;
        }
        g.queue[(g.queue_head + i) % kMaxScheduledJoystick] = prev;
        i--;
    }
    g.queue[(g.queue_head + i) % kMaxScheduledJoystick] = input;
    g.queue_count++;
    return true;
}

} // namespace tisple
