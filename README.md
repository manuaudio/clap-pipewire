# clap-pipewire

Run [CLAP](https://github.com/free-audio/clap) audio plugins as system-wide
PipeWire effects: on an output device, or as a virtual sink or source of their
own. PipeWire's `filter-chain` already hosts LADSPA, LV2 and its built-in
filters. This project adds a `type = clap` node:

```
{ type = clap  name = fx  plugin = "lsp-plugins.clap"  label = "in.lsp-plug.filter_stereo" }
```

It's a small C library that builds against the system PipeWire headers and
installs next to PipeWire's own filter-graph plugins. You don't patch or
rebuild PipeWire, and you don't need root.

**Status: v0.1, working and measured, early.** It supports audio effects,
with parameters as filter-chain controls, latency reporting and plugin state.
It targets **PipeWire 1.6.x** and is measured live on 1.6.8 with LSP Filter
Stereo. It also passes `clap-pw-info -c` with Dragonfly Room and Plate,
Surge XT Effects, CHOW Tape Model and Airwindows Consolidated. Plugins run
**in process** (see [Limits](#limits)).

This is an independent project. It is not part of PipeWire and is not meant
to become an upstream merge request: it was developed with AI assistance (see
the commit trailers), which PipeWire's contribution policy doesn't accept.

## How it hooks into PipeWire

```
module-filter-chain
  └─ libspa-filter-graph.so            builds the graph from filter.graph = { ... }
       └─ type = clap  →  <SPA_PLUGIN_DIR entry>/filter-graph/libspa-filter-graph-plugin-clap.so
            └─ factory "filter.graph.plugin.clap"   (this project, src/plugin_clap.c)
                 └─ CLAP host core                  (src/clap-host.c, no PipeWire code)
                      └─ ~/.clap/lsp-plugins.clap   dlopen + clap_entry
```

- filter-graph resolves `type = X` to `filter-graph/libspa-filter-graph-plugin-X`
  and looks it up in every directory of `SPA_PLUGIN_DIR`, which is a
  colon-separated list. That makes an out-of-tree plugin type possible
  without changing PipeWire. `clap-pw-host` sets the variable for you:
  `~/.local/lib/spa-0.2:/usr/lib/spa-0.2`.
- The plugin interface, `spa/plugins/filter-graph/audio-plugin.h`, is
  **not installed** by PipeWire and isn't a stable ABI. A copy from 1.6.8 is
  vendored in `src/fga/`, unchanged. PipeWire master (1.7) has already
  changed it (`instantiate()` takes all handles at once, and `set_io` and
  `control_sync` are new) without bumping its version number. So the plugin
  checks which PipeWire it was loaded into and **refuses to load outside
  1.6.x** rather than crash the process. CI diffs the vendored header against
  the release it builds with, and warns when master differs.
- The CLAP host core (`src/clap-host.[ch]`) knows nothing about PipeWire. The
  SPA glue only maps its ports onto filter-graph ports. A later
  out-of-process worker can reuse the same core.

### Mapping

| CLAP | filter-graph |
|---|---|
| bundle (`.clap` file, or a DPF bundle directory) | `plugin =`: an absolute path, or a name in `$CLAP_PATH`, `~/.clap`, `/usr/lib/clap`, `/usr/local/lib/clap` |
| plugin id (or name) | `label =`. It can be empty when the bundle holds a single plugin |
| audio ports | audio ports `in_1`, `in_2`, …, `out_1`, …. Extra CLAP ports (sidechains) become `in2_1`, `in2_2`, … |
| writable params | controls, named after the CLAP param name made safe (`Gain (dB)` → `Gain_dB`). Clashing names get `_<id>`. Stepped params become integer or boolean |
| read-only params | notify (output control) ports, updated from the plugin's output events |
| latency extension | a `latency` notify port with `SPA_FGA_HINT_LATENCY`, so filter-chain reports it |
| state extension | `config = { state = … save-state = … }` |
| hidden params | not exposed |

Control values are the plugin's own parameter values. Some plugins use
normalized 0..1 values, LSP for frequencies for example.
`clap-pw-info … NAME=TEXT` converts a value as the plugin displays it into
the number a control wants.

## Build and install

You need PipeWire 1.6 development headers (`libpipewire` on Arch,
`libpipewire-0.3-dev` / `pipewire-devel` elsewhere), meson ≥ 0.63, ninja and
a C compiler. The CLAP SDK (MIT, header-only) comes in as a meson wrap
(`subprojects/clap.wrap`, tag 1.2.10), unless a system `clap` dependency is
found.

```sh
meson setup build --prefix="$HOME/.local"
meson test -C build                # unit tests, no PipeWire daemon needed
meson install -C build
```

This installs, all user-local:

| file | |
|---|---|
| `~/.local/lib/spa-0.2/filter-graph/libspa-filter-graph-plugin-clap.so` | the filter-graph plugin |
| `~/.local/bin/clap-pw-info` | list plugins, ports and parameters as filter-graph will name them |
| `~/.local/bin/clap-pw-host` | run a filter-chain config as its own PipeWire client with the plugin on `SPA_PLUGIN_DIR` |
| `~/.local/share/systemd/user/clap-pw-host@.service` | optional unit template (not enabled) |
| `~/.local/share/clap-pipewire/examples/` | `clap-sink.conf`, `clap-smart-filter.conf` |

## Use

**1. Pick a plugin and see its ports and parameters.**

```sh
clap-pw-info                                   # every bundle in the CLAP path
clap-pw-info lsp-plugins.clap                  # the plugins in one bundle
clap-pw-info lsp-plugins.clap in.lsp-plug.filter_stereo
clap-pw-info lsp-plugins.clap in.lsp-plug.filter_stereo Frequency=500
# "Frequency" = 0.502622485    # 500 Hz
clap-pw-info -c lsp-plugins.clap in.lsp-plug.filter_stereo   # also activate and run 2 s of a tone
```

Each probe runs in a child process, so a plugin that crashes or prints to
stdout doesn't take the listing down. `-c` checks that the output is finite
and not silent, and reports latency and process errors.

**2. Write a filter-chain config.** A node looks like this:

```
{
    type    = clap
    name    = fx
    plugin  = "lsp-plugins.clap"
    label   = "in.lsp-plug.filter_stereo"
    control = { "Filter_type" = 0  "Filter_slope" = 3  "Frequency" = 0.502622485 }
    config  = {
        params     = { "Output_gain" = "-3 dB" }    # once at start, via the plugin's text_to_value
        state      = "/home/me/.config/clap-pipewire/fx.state"   # clap.state, loaded at start
        save-state = true                            # written back when the effect stops
        max-block  = 8192                            # CLAP max_frames_count; larger blocks are split
    }
}
```

Two complete examples ship with it:

- `examples/clap-sink.conf` creates a virtual output, "CLAP effect sink".
  Whatever plays to it goes through the plugin to the default output.
- `examples/clap-smart-filter.conf` puts the effect **on a real device** as a
  WirePlumber (≥ 0.5) smart filter. Every stream WirePlumber links to that
  device goes through the plugin, and the application doesn't know. Set
  `filter.smart.target` to your device's `node.name` first.

**3. Run it.**

```sh
clap-pw-host ~/.local/share/clap-pipewire/examples/clap-smart-filter.conf
# or keep configs in ~/.config/clap-pipewire/NAME.conf and
systemctl --user enable --now clap-pw-host@NAME
```

Each effect host is its own PipeWire client. Starting or stopping one
doesn't restart the audio server, and stopping the process removes the
effect.

**4. Control it live.**

```sh
pw-cli set-param <node-id> Props '{ params = [ "fx:Output_gain" -6.0 ] }'
pw-dump <node-id>                                  # Props show the current values
pw-metadata -n filters <node-id> filter.smart.disabled true   # system bypass (smart filter)
pw-metadata -n filters -d <node-id> filter.smart.disabled     # back on
```

Parameters are filter-chain controls, so `pw-cli`, `wpctl`-based tools and
any PipeWire client that writes `Props` can change them. `pw-metadata` is the
on/off switch for the smart filter; it doesn't set plugin parameters.

**Inside the main PipeWire daemon (not recommended).** If you export
`SPA_PLUGIN_DIR=$HOME/.local/lib/spa-0.2:/usr/lib/spa-0.2` to `pipewire.service`
(an `environment.d` file), `type = clap` also works in
`~/.config/pipewire/pipewire.conf.d/` filter-chains. But a crashing plugin
then takes down all audio. With `clap-pw-host`, it only takes down that one
effect.

## Measured proof

`tools/live-proof` does the whole thing on a live session without touching
real devices. It creates a throwaway null-sink "device", plays a known tone
**at the device**, records the device's monitor through explicit links,
inserts LSP Filter Stereo (CLAP) as a smart filter, changes parameters live,
then removes everything and checks that nothing is left.

Results on PipeWire 1.6.8 and WirePlumber 0.5, with LSP Plugins 1.0.25
(CLAP 1.2.10). The tone is −12 dBFS peak (−15.1 dBFS RMS), and the filter is
low-pass, slope ×4, 500 Hz:

| step | player routed to | 200 Hz | 5 kHz |
|---|---|---|---|
| no effect | device | −15.1 | −15.1 |
| CLAP effect loaded (smart filter) | `clap-pw-proof.fx` | −20.2 | **−77.1 (−62 dB)** |
| system bypass (`filters` metadata) | device | | −15.1 |
| bypass cleared | `clap-pw-proof.fx` | | −77.1 |
| live `fx:Output_gain = -12` | `clap-pw-proof.fx` | **−32.2 / −32.2** (exactly −12 dB, both channels) | |
| live `fx:Frequency` → 8 kHz | `clap-pw-proof.fx` | | L −25.4 / R −48.3 (see note) |
| live `fx:Bypass = 1` (plugin's own) | `clap-pw-proof.fx` | | −15.1 |
| host stopped | device | | −15.1; filter nodes gone |
| device removed | | | 0 test nodes left, default sink unchanged |

The CLAP result is identical to the LV2 build of the same plugin loaded
through filter-chain's `lv2` type (−77.1 at 5 kHz).

Latency also reaches the graph. CHOW Tape Model reports 39 samples through
CLAP's latency extension, and the filter-chain node then publishes
`ProcessLatency { rate: 39 }`.

**Note on the 8 kHz row.** After a *live* cutoff change, LSP Filter Stereo's
right channel lags: −48.3 instead of −25.4. The LV2 build shows exactly the
same numbers. Driving the CLAP build offline through the host core, with no
PipeWire involved, reproduces it as well, while a fresh instance loaded at
8 kHz reads −25.4 on both channels. So it's in the plugin, not in PipeWire
or in this host: one parameter event reaches one plugin instance for both
channels. Other live changes (gain, bypass) are symmetric.

`tools/check-examples` runs both shipped example configs through the
installed `clap-pw-host` against a throwaway device.

## Tests

```sh
meson test -C build           # host core + SPA plugin + CLI, against a test CLAP plugin
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false && meson test -C build-asan
tools/live-proof              # needs a running PipeWire + WirePlumber, jq, python3, lsp-plugins.clap
tools/check-examples          # needs the plugin installed
```

The unit tests use a small CLAP effect built from `tests/test-plugin/`. It
checks that the host follows CLAP's thread and lifecycle rules (main-thread
`activate`/`state`/`destroy`, audio-thread `start_processing`/`process`/`stop_processing`,
no in-place buffers without `in_place_pair`, global param events with the right
cookie), and counts every violation. The tests cover:

- the search path and bundle resolution;
- the port layout and name clashes;
- processing, block splitting and aliased buffers;
- defaults versus state versus config values;
- restart requests and latency;
- `on_main_thread`, and logging from the audio thread;
- state round-trips, text values;
- freeing an instance while a main-thread callback is still queued;
- the PipeWire-series guard;
- the full filter-graph descriptor lifecycle through the built `.so`.

## Realtime behaviour

What runs on the audio thread (`run()` from filter-graph, the CLAP
`process()` call) uses only memory allocated up front. It takes no locks
and makes no blocking calls:

- **Parameters.** Changes are found by comparing each control with the last
  value sent, and become `CLAP_EVENT_PARAM_VALUE` events at frame 0 from a
  preallocated array. Filter-graph writes controls from the main loop; they
  are read with relaxed atomics.
- **Blocks.** Blocks longer than `max-block` are split. If an input aliases
  an output, it's copied into preallocated scratch first.
- **Plugin requests.** `request_callback`, `request_restart` and
  `params.request_flush` are queued to the main loop with PipeWire's own
  non-blocking `spa_loop_invoke`, at most once per pending request.
  `on_main_thread`, `activate`/`deactivate` and `state` always run on the
  main loop.
- **Restarts.** A restart is a lock-free handshake: main asks the audio side
  to stop, audio calls `stop_processing` and hands back, main deactivates and
  reactivates. Audio passes through dry meanwhile.
- **Plugin logging.** Log lines from the audio thread go into a fixed ring
  and are printed from the main loop.
- **Teardown.** `stop_processing` on teardown is run on the data loop
  (blocking invoke), so it stays on the audio thread as CLAP requires.

Realtime scheduling of the effect host comes from `libpipewire-module-rt`,
as for any PipeWire client.

## Limits

- **In-process.** The plugin runs inside the PipeWire client that hosts the
  filter-chain. A crash, hang or memory corruption in the plugin kills that
  process. With `clap-pw-host` that means one effect, and the device falls
  back to dry playback (`node.dont-fallback` removes the filter instead of
  rerouting). Inside the main daemon, it means all audio. True isolation
  needs the out-of-process host below.
- **Effects only.** There are no note ports, so instruments and note effects
  don't get MIDI.
- **No GUI.** The plugin's own GUI is never opened. Parameters are set
  through controls.
- **Fixed layout.** The parameter and port layout is fixed when the effect
  loads. A plugin that rescans its parameter list or audio ports gets a
  warning; reload the filter to pick up the change.
- **Latency.** Latency is reported when the graph activates. A latency
  change after a plugin-requested restart is applied inside the plugin, but
  filter-chain only recomputes graph latency on its next activation.
- **Not yet supported:**
  - 64-bit buffers;
  - transport info;
  - the `thread-pool`, `timer-support`, `posix-fd-support`, `voice-info`
    and `preset-load` extensions;
  - `CLAP_PROCESS_SLEEP` (the plugin is called every cycle).
- **State and defaults.** A control left at its default isn't sent, so a
  state file or `config.params` value survives. The flip side is that
  `Props` then shows the default for that control, not the restored value.
- **PipeWire 1.6.x only**, because of the uninstalled plugin ABI described
  above.

## Next steps

1. **Out-of-process hosting.** A worker process per plugin instance, driven
   over shared memory by a thin filter-graph stub, or each worker its own
   `pw_filter` node. `clap-host.c` is already PipeWire-free for that. A
   crash then becomes a dropout plus a restart, not a dead host.
2. **Parameter UI without plugin GUIs.** Filter-chain already publishes
   `PropInfo` (names and ranges). Add the plugin's `value_to_text` and
   parameter modules, and a generic panel (TUI, web, or a desktop shell
   widget) can show and edit any CLAP effect.
3. **Presets.** Index CLAP `preset-discovery` providers and load presets
   through `preset-load`, alongside the plain `state` files.
4. **Instruments and note effects.** Map filter-graph sequence ports
   (`SPA_FGA_PORT_SEQUENCE`) to CLAP note ports.
5. **PipeWire 1.7 ABI.** Add a second glue for the new `instantiate(n_hndl)`
   and `control_sync` interface, chosen at build time.

## License

MIT, see [LICENSE](LICENSE). `src/fga/audio-plugin.h` is PipeWire's (MIT).
The CLAP SDK (MIT) is fetched at build time.
