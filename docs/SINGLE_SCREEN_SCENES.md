# Single-screen scene research

The Godot single-screen layout is configurable presentation, not automatic scene detection. Its scene detector is a stub, and the default retains the entire bottom screen as an overlay. Some scenes use the bottom screen for gameplay rather than a removable panel; cropping based on an unvalidated guess can hide essential content or touch targets.

Exploratory local research considered overlay residency, state and pointer candidates, and sampled visual observations. None established a reliable scene classifier: overlays can coexist or remain resident across different scenes, state may be transitional, and a flag associated with text processing does not necessarily indicate visible dialogue. A candidate that appears precise on a small sample can still miss entire scene phases. These observations are not a validated rule or a general parity claim.

Any future classifier should use coherent read-only post-step snapshots, validate ranges and alignment before following pointers, and treat missing, stale, ambiguous or transitional data as unknown. A snapshot taken after a step can differ in timing from pixels scanned earlier in that frame. Preserve an explicit fallback that retains essential bottom-screen content; independently validate both false positives and missed scenes before enabling rules. Local captures, memory dumps, generated code and other game-derived material must not be published.

See [Building](BUILDING.md) for shape configuration and touch routing.
