# Scene detection for presentation

`DqMachine.get_scene()` exposes a read-only guest-state classification for host
presentation; `scene_changed(scene)` reports sampled changes on Godot's main
thread. See [the Godot API](../godot/API.md) for values, delivery and lifecycle
semantics. The classifier does not change guest memory, emulation or layout.
`OTHER` includes unrecognized and unavailable state; it is not a positive field,
menu or dialogue classification. Consumers must retain a safe presentation for
OTHER rather than assuming an unclassified scene is gameplay.

Godot signals are presentation-timed, not guest-frame-boundary callbacks. A
future guest-camera consumer must apply scene changes on the owner thread at a
guest-frame boundary, rather than changing guest camera state from the signal.

Godot's presentation rules advance on every completed emulated frame, including
frames replaced before display. Their resulting label, main-screen choice and
darkness sample travel with the displayed frame; a fast replay paused at a
target therefore uses the same automatic rule state as sequential presentation.
File tracking supplies the last tracked read within each frame, not every read
within that frame. These rules do not change the guest or make the underlying
scene heuristics frame-exact.

The battle candidate follows the live ov017 world singleton at `0x021d82e4`
through world `+0x3718`, owner `+0x6ac` and dispatcher `+0xea8`. Overlay
residency and valid pointer reads matter; the stale ov000 global is not proof
of a live owner. Character creation uses overlay 21, an overlay lifecycle
indicator rather than a pixel-exact boundary of visible content. Creation
requires an idle tracker. Battle can be classified while the tracker is busy
when overlay 17 is resident, the tracker row is valid, no slot in that row has
an unload mark, and the valid owner chain has a nonzero phase. Guest unload
routines mark the slot before setting busy; this remains a lifetime heuristic,
not a guarantee of exact visual transitions. Invalid tracker state or invalid
nonzero battle-chain pointers produce OTHER. Creation takes precedence over
battle.

The owner destructor has an interval in which the allocator has been destroyed
but owner `+0x6ac` has not yet been cleared. The pointer chain can therefore
still appear valid during teardown; this is a known risk of the lifetime
heuristic, not proof that the allocator remains live.

`INTRO` and `TITLE` are reserved but are not currently classified: absent a
different established class, they remain OTHER. The previously explored intro
mode-byte heuristic produced false positives in creation and credits; title
overlay 16 can be resident while the opening movie plays. INTRO and TITLE have
not been validated for automatic presentation switching.

Prior investigation found the
battle rule promising on sampled known-label routes, but did not establish
frame-exact transitions or a full-frame false-negative rate. Creation samples
were concentrated on the startup route, and title overlay residency was not
equivalent to a visible title prompt. Unknown and fade frames were not fully
labelled. These observations are not independent validation of automatic layout
switching; do not treat the classifier as proof that a scene is visually safe
to crop or widen.

The single-screen layout remains configurable presentation; its default retains
the entire bottom screen as an overlay. Some scenes use that screen for
gameplay rather than a removable panel. See [Building](BUILDING.md) for shape
configuration and touch routing. Local captures, memory dumps, generated code
and other game-derived material must not be published.
