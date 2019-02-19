// -*- c++ -*-

// TODO: decide the file names
#include "glukeys/Glukeys.h"

#include <Arduino.h>

#include <kaleidoglyph/Key.h>
#include <kaleidoglyph/KeyAddr.h>
#include <kaleidoglyph/KeyArray.h>
#include <kaleidoglyph/KeyEvent.h>
#include <kaleidoglyph/KeyState.h>
#include <kaleidoglyph/Plugin.h>
#include <kaleidoglyph/hid/Report.h>
#include <kaleidoglyph/cKey.h>


namespace kaleidoglyph {
namespace glukeys {

// Event handler
EventHandlerResult Plugin::onKeyEvent(KeyEvent& event) {

  // Ignore all `injected` events
  if (event.state.isInjected()) {
    return EventHandlerResult::proceed;
  }

  if (event.state.toggledOn()) {

    // This key can't be `pending` if it's toggling on (unless another plugin is injecting
    // this event), so if its `temp` bit is set, that means it's already in the `sticky`
    // state, so this is the second press of the key, and we should therefore lock it.
    if (isTemp(event.addr)) {
      // `sticky` => `locked`
      clearTemp(event.addr);
      return EventHandlerResult::abort;
    }

    // We have already eliminated the possibility of this key being in either the
    // `pending` or `sticky` state, that means if its `sticky` bit is set, the key is
    // `locked`. This is the third press, so we should clear it (and we don't need to
    // clear `temp`, because we know it's already clear).
    if (isSticky(event.addr)) {
      // `locked` => `clear`
      clearSticky(event.addr);
      return EventHandlerResult::abort;
    }

    // If there's already a release trigger set, that means previously-set `sticky`
    // glukeys need to be released before we proceed, or they would continue to be active
    // past the key that should have released them.
    if (release_trigger_ != cKeyAddr::invalid) {
      releaseAllTempKeys();
      release_trigger_ = cKeyAddr::invalid;
    }

    // Determine if the pressed key is a glukey
    const Key glukey = lookupGlukey(event.key);

    // If it's not a GlukeysKey...
    if (glukey == cKey::clear) {
      // This `event.key` is not a glukey. Check to see if this key should be set as the
      // trigger for release of `sticky` glukeys (and clearing of `pending` ones). Certain
      // types of keys (modifiers & layer changes) shouldn't trigger release, and we
      // should only set the trigger if there are any glukeys in the `pending` or `sticky`
      // states.
      if ((temp_state_count_ != 0) && isTriggerCandidate(event.key)) {
        release_trigger_ = event.addr;
        // Also, release any `sticky` layer-shift glukeys. The layer shift has already
        // been applied to this trigger key (`event.key` was looked up from the shifted-to
        // layer), and we don't want the layer shift to persist beyond that.
        if (layer_shift_addr_ != cKeyAddr::invalid) {
          KeyEvent event{layer_shift_addr_, cKeyState::injected_release};
          controller_.handleKeyEvent(event);
          layer_shift_addr_ = cKeyAddr::invalid;
        }
      }
      return EventHandlerResult::proceed;
    }
    // If it's a GlukeysKey, but its index value is out of bounds, abort
    if (glukey == cKey::blank) {
      return EventHandlerResult::abort;
    }
    // Change the `event.key` value to the one looked up in the `glukeys_[]` array of
    // `Key` objects (and let Controller restart the onKeyEvent() processing
    event.key = glukey;
    // `clear` => `pending`
    setTemp(event.addr);
    // Clear the release trigger whenever a glukey is pressed to prevent a bug caused by
    // rollover from the previous (unreleased) trigger key when a new glukey is
    // pressed. If we don't do this, the previous trigger key's release will also release
    // the newly-pressed glukey before its trigger is pressed.
    release_trigger_ = cKeyAddr::invalid;
    return EventHandlerResult::proceed;
  }

  if (event.state.toggledOff()) {
    // If the key is `pending`, make it `sticky`
    if (isTemp(event.addr)) {
      // `pending` => `sticky`
      setSticky(event.addr);
      // If this is a layer-shift key, record its address for later release:
      if (isLayerShiftKey(event.key)) {
        layer_shift_addr_ = event.addr;
      }
    }
    // If the key is either `sticky` or `locked`, stop the release event
    if (isSticky(event.addr)) {
      return EventHandlerResult::abort;
    }
    // If the trigger key was released, also release any `sticky` glukeys
    if (event.addr == release_trigger_) {
      releaseAllTempKeys();
      release_trigger_ = cKeyAddr::invalid;
    }
  }

  return EventHandlerResult::proceed;
}


// Check to see if the `Key` is an Glukeys key and if so, return the corresponding
// (looked-up) `Key` value, or `cKey::clear` if there is none.
inline
const Key Plugin::lookupGlukey(const Key key) const {
  if (GlukeysKey::verify(key)) {
    byte glukey_index = GlukeysKey(key).index();
    if (glukey_index < glukey_count_) {
      return getProgmemKey(glukeys_[glukey_index]);
    }
    // Indicator for an invalid index
    return cKey::blank;
  }
  // Indicator for a non-GlukeysKey
  return cKey::clear;
}


// Clear all `pending` keys and release all `sticky` keys
void Plugin::releaseAllTempKeys() {
  for (byte r = 0; r < state_byte_count; ++r) {
    // We expect that most keys won't have any glukey bits set
    if (temp_state_[r] == 0) continue;

    // Store a bitfield of the keys that need to have release events sent
    byte sticky_glukeys = temp_state_[r] & sticky_state_[r];

    // Unset all sticky bits of keys with the temp bit set. This means that any keys in
    // the `sticky` state will end up `clear`, not `locked`:
    sticky_state_[r] &= ~temp_state_[r];

    // Clear all temp bits. This means that all keys that were `pending` become `clear`:
    temp_state_[r] = 0;

    // Send release events for sticky keys:
    for (byte c = 0; c < 8; ++c) {
      if (bitRead(sticky_glukeys, c)) {
        // Send release event
        KeyAddr k { byte(r * 8 + c) };
        KeyEvent event {
          k,
          cKeyState::injected_release
        };
        controller_.handleKeyEvent(event);
      }
    }
  }
  temp_state_count_ = 0;
  // It would make sense to clear the release trigger key here, as well, but it's not
  // always necessary, because in the normal case, it's not possible to get a release of
  // the trigger key until after it is pressed again. See comment above where
  // releaseAllKeys() is called.
}


// Test for types of keys that are eligible to trigger release of `sticky`
// glukeys. Returns `true` if `key` will trigger release of `sticky` glukeys.
bool isTriggerCandidate(const Key key) {
  // Modifiers never trigger release of `sticky` glukeys
  if (KeyboardKey::verify(key)) {
    return ! KeyboardKey(key).isModifier();
  }
  // Layer change keys of all kinds also don't release `sticky` glukeys
  if (LayerKey::verify(key)) {
    return false;
  }
  return true;
}


} // namespace glukeys {
} // namespace kaleidoglyph {
