# Native Qt previews

These PNGs were captured from the actual `ActivityStatus` widget, in the dock's Native Plus style, by its optional Qt
test harness. The harness feeds explicit test lifecycle events and uses palettes in Yami and Yami Light colours, with
the pill on the dock's base colour; production activity comes from `ClipFiler`. They are not OBS screenshots or
evidence of Windows runtime validation.

| State | Dark | Light |
|---|---|---|
| Ready | ![Ready](dark-ready.png) | ![Ready](light-ready.png) |
| Waiting | ![Waiting](dark-waiting.png) | ![Waiting](light-waiting.png) |
| Working | ![Working](dark-working.png) | ![Working](light-working.png) |
| Expanded | ![Details](dark-expanded.png) | ![Details](light-expanded.png) |
| Complete | ![Complete](dark-completed.png) | ![Complete](light-completed.png) |
| Failure | ![Failure](dark-failed.png) | ![Failure](light-failed.png) |
| Failure details | ![Failure details](dark-failed-expanded.png) | ![Failure details](light-failed-expanded.png) |
| Narrow | ![Narrow](dark-narrow.png) | ![Narrow](light-narrow.png) |

Regeneration commands and remaining validation are in [the handover](../handover/activity-status.md).
