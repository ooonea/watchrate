# Watchrate

Native Qt 6 / Wayland watch offset tracker. Initial prototype, with Italian UI.

Choose a watch and arm the measurement. Press Space once, or press Stop, and
remember the time its hands indicated at that instant. The reference freezes.
Enter the watch's local date and time afterwards, then save. Time spent entering
this reading never affects the captured reference. The displayed reference and
its Chrony status remain those of the stop, even if synchronization changes later.

Acquisition occurs on key/button press, not release. Auto-repeat cannot create
additional samples. Escape or Discard cancels the measurement. Loss of application
focus, a detected clock step or suspend cancels an armed measurement; a reference
already captured remains available for entry after focus changes. The watch
selection cannot change until the measurement is saved or discarded. Reading
entry is enabled only after Stop. Stop on a known whole-second landmark to avoid
rounding an arbitrary hand position to the second-resolution input.

Positive offset means the watch is fast. The reference is CLOCK_REALTIME,
disciplined externally by Chrony, not a direct connection to an atomic clock.
Chrony must report a synchronized external reference. Its estimated clock-error
bound is saved with each sample: absolute system correction + root dispersion
+ half root delay. The UI checks Chrony every five seconds and rejects status
older than fifteen seconds. This is a snapshot, not a certified error bound.
No privileged command or clock adjustment is performed.

Nanosecond timestamp storage and millisecond display are resolution, not accuracy.
Human reaction, keyboard/pointer delivery and event-loop latency are not corrected.
The displayed clock is refreshed for readability; its drawn frame is not the
measurement source. Consistent observation over long intervals reduces the
influence of variable reaction time on rate estimates. It does not calibrate
absolute offset. Mechanical watches benefit from daily readings; quartz watches
usually need longer intervals. The app does not measure acoustic beat rate.

Mean rate uses first and last samples in the current series, with actual elapsed
UTC time, and is shown only for intervals of at least an hour. After setting the
watch, start a new series. Historic samples remain available. The graph covers
the current series; the table and CSV cover all series for the selected watch.
Ambiguous local times during DST transitions should be avoided in this prototype.

## Build and run

`nix build` runs the included tests and creates `result/bin/watchrate`.
`nix run` launches it. The flake pins the same nixpkgs revision used by unicorn
when the prototype was created. This is a standalone application; it does not
modify the workstation flake.

The database is stored beneath Qt's XDG application data directory
(`$XDG_DATA_HOME/ooonea/watchrate/watchrate.sqlite` by default). CSV timestamps
are UTC. The app runs without a background service or listener.

## Noctalia / Umbriel

The desktop entry and live Wayland app_id are `dev.ooonea.Watchrate`.
The app uses native Qt widgets, inherited fonts and themed icons, with compositor
decorations. It reads the Noctalia-generated `kdeglobals` color groups and reloads
changed colors without restart. This reuses the existing kcolorscheme template.
An optional `$XDG_CONFIG_HOME/watchrate/theme.ini` overrides these colors;
`data/noctalia-theme.ini.in` is a Noctalia user-template input for that option.
No template or compositor rule is installed by running the app.

The icon uses the system `chronometer` icon. No custom raster asset is generated.
Installing the package into the desktop environment is needed for launcher and
dock desktop-entry lookup; a direct preview alone cannot verify those features.

## Validation

`watchrate --self-test` checks offset signs, midnight arithmetic, clock-step and
suspend detection, Chrony CSV parsing, SQLite insertion, single-key capture, frozen-reference save,
auto-repeat suppression, focus-loss cancellation, known daily rate and RGB-list
theme reload. CTest runs it offscreen.
Live Wayland identity, visible layout and owner acceptance are separate checks.
