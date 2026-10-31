# Home: replacing LG's launcher

`home` is a Home screen in place of LG's: one horizontal ribbon along the
bottom of the screen, laid over whatever is playing, with the apps, the HDMI
inputs, LG's own screens that open over everything (the input picker,
notifications, ...), and a small panel for picture mode, sound output and
energy saving. It is not a port of
[glasshouse](https://github.com/rorygallagher2024/glasshouse)'s Home; that
project's research is where most of what follows comes from, and it is cited
where it was measured on a TV other than ours. Nothing in this file has been
checked on `10.10.8.63` yet; see [What to check first](#what-to-check-first).

```sh
cmake --build build --target home-install
ssh root@10.10.8.63 sh -s take    < tools/home-takeover.sh   # LG's Home -> ours
ssh root@10.10.8.63 sh -s restore < tools/home-takeover.sh   # and back
```

Taking over is **non-permanent by construction**: the copy and the bind mount
live in `/tmp`, so a reboot gives LG's Home its place back whatever state
anything is left in. `restore` does the same without the reboot.

## How the takeover works

LG's Home is `com.webos.app.home`, and it is what the TV opens for everything
that means Home: the Home key (surface-manager, `launchType: homeKey`), Back
out of the last app (sam's `last_input_handler`), waking from standby
(bootManager). Nothing calls it as a Luna service. So the replacement takes its
**id** rather than any setting:

1. Copy the installed `dev.hookedbehemoth.home` to `/tmp/home-takeover`, with an
   `appinfo.json` naming `com.webos.app.home`, `main` as an absolute
   `file://` path as LG's own manifest has it, `appCategory: ["home"]` (without
   it webOS 10's Home key opens nothing) and `defaultWindowType: "overlay"` (not read for a native app, see below).
2. `mount --bind` it over `/usr/palm/applications/com.webos.app.home`. The
   mount propagates into the voice service's jails, which see that directory
   too.
3. Make sam read that manifest again. sam reads `appinfo.json` only when it
   scans, and has no call for it, so:
   - webOS 10 and 11: `applicationManager/blockSystemApp` then
     `releaseBlockSystemApp` on the id rereads that one app (webOS 11 wants
     `forceBlockSystemApp: false`, named only in its schema);
   - webOS 7 to 9: configd's `profile.blockedAppList` set to itself plus an id
     no app has, then back, makes sam rescan everything without restarting.
     configd keeps that value across reboots as a user value: it is LG's own
     list, but a later change of LG's list would not be seen.
   - webOS 5 and 6 have neither; only restarting sam works there, which closes
     every app, and the script does not do it.
   - **webOS 9.2.2 (our TV) has `blockSystemApp` but not
     `releaseBlockSystemApp`** ("Unknown method"), and its schema has no
     `forceBlockSystemApp`. A block there lasts until sam restarts: Home then
     opens nothing (`getAppInfo`: "Invalid appId specified OR Unsupported
     Application Type") and only a reboot undoes it; nothing is written to
     disk. The script therefore nudges first, and blocks only where a release
     for an unblocked id answers true.
   - Where configd held no `profile.blockedAppList`, the nudge leaves it holding
     the country's list (`blockedAppList/<country>.json`), so sam blocks what
     it did before.
4. Only then stop LG's running Home (its process, or on webOS 10/11 the
   `flutter-client -i com.webos.app.home` runner). Changing the manifest's type
   under a running instance leaves sam unable to close or replace it.
5. webOS 11's preload manager starts LG's Home again in the background;
   `preloadmanager/setPreloadPolicy` with `isEnabled: false` stops that, and
   `restore` turns it back on.

`restore` unmounts, rereads, and stops our process last; where ours was in
front, sam opens LG's Home in its place.

## What it took on webOS 9.2.2

Checked on `10.10.8.63` (Rockhopper 9.2.2) on 31 October 2026; with all of it
the launcher registers with sam, every call below is answered, and the ribbon
draws the TV's apps with their icons.

- **The executable, not only the manifest.** ls-hubd gives the name
  `com.webos.app.home`, with LG's Home's `privileged` role and outbound access
  to every service, only to `/usr/bin/com.webos.app.home`
  (`/usr/share/ls2/roles/{pub,prv}/com.webos.app.home.json`). Run from
  anywhere else the bus refuses it, sam never hears from it and keeps its
  launch spinner up (`spinnerOnLaunch`) for good. The takeover mounts the
  binary over that file too, and the manifest's `main` names it.
- **No `APPID` for a system app.** SAM starts it with the id only in the
  launch payload (`argv[1]`, `"appId"`). Without it the launcher named itself
  by its development id, to the bus and to the compositor alike.
- **Not through libhelpers.** libhelpers works the app's name out from where
  its executable is and, from `/usr/bin`, fails every call before reaching the
  bus, with an empty `HGetError`. The launcher registers the name itself with
  luna-service2 (`jf_luna_set_name`); jellyfin keeps libhelpers.
- **Stop the old instance after the reread.** sam's last-input handler starts
  Home again at once, from whatever manifest it holds at that moment.
- **The launcher runs as root**, unjailed (`privilegedJail: false`).
- `/tmp/criu_enable` is set, and sam made a (still empty) checkpoint directory
  for the launcher: Home is one of the apps it snapshots. Whether a restore
  from such a snapshot keeps the bus connection is not known.
- The bind mounts propagate into every jailed app's view of `/usr/bin` and
  `/usr/palm/applications`; unmounting takes them all down.

## Drawing over the app beneath

LG's Home before webOS 6 was a ribbon over the running app, and the launcher
draws the same: the frame is cleared to transparent and only the ribbon, a
scrim behind it and the panels are drawn. Through SDL it was a card on webOS
9.2.2 - the app beneath was sent to the background, paused, and did not show
through - which is what this section is about.

The compositor decides by the surface's `_WEBOS_WINDOW_TYPE` property. LG's
screens that draw over a playing input - Settings, Notifications, Search - are
`overlay` in their manifests (they are web apps; WebAppMgr sets the property
from it). glasshouse saw LG's Home as `_WEBOS_WINDOW_TYPE_HOME` on webOS 5,
where the app beneath kept playing. What was found for a native app on
webOS 9.2.2 (surface-manager logs each window's type as `LSM NL_VSC`):

- **The manifest is not read.** `defaultWindowType` `home` or `overlay` in the
  mounted manifest still gave `_WEBOS_WINDOW_TYPE_CARD`.
- **The TV's SDL 2.0.14 can set the type, at a price.** Its
  `Wayland_CreateWindow` sets `_WEBOS_WINDOW_TYPE` to the hint
  `SDL_HINT_WEBOS_WINDOW_GROUP_TYPE` (the hint names really are spelled with
  `SDL_HINT_`), but only on the branch for a window that owns a surface group:
  `SDL_HINT_WEBOS_WINDOW_GROUP_OWNER` set and not `"false"`, after which it
  creates that group and attaches the window to it as
  `SDL_HINT_WEBOS_WINDOW_LAYER_NAME`. With those hints the launcher, started
  by SAM, was killed outright (SIGKILL: no signal, no exit, no crash report)
  within a second of showing - with the type hint or without it. Run by hand
  it survived, so it is what happens when the window is shown.
- **SDL does not hand out the webOS shell surface.** `SDL_GetWindowWMInfo`
  gives a plain `wl_shell_surface` (checked with `wl_proxy_get_class`), and
  SDL loads libwayland with `dlopen`/`dlsym`, so its calls cannot be
  interposed to catch the `wl_webos_shell_surface` it makes.

**So the launcher draws its own window on the TV**, through
`platform/window_wayland.c`, the second implementation of `window.h`. It
opens libwayland-client at runtime, finds each request by name in the
interface's own method table, and uses `wl_proxy_marshal_flags` only where it
exists - that call is libwayland 1.20's, and the shim this project once had
used it alone, which is what kept it off older TVs. Everything else is the
calls the TV's own SDL 2.0.14 looks up. With `_WEBOS_WINDOW_TYPE_OVERLAY` set
on the shell surface before the first frame, surface-manager logs the window
as `_WEBOS_WINDOW_TYPE_OVERLAY`, it survives being launched by SAM, the app in
front stays the foreground app (`getForegroundAppInfo`), and a capture shows it
through and around the ribbon (checked over YouTube on 31 October 2026). It
has no on-screen keyboard and no exported video window yet, so jellyfin keeps
`window.c`; off the TV the launcher uses `window.c` too, for its tests.

**Hiding is unmapping.** Back on the launcher's first screen, and opening
anything from it, put it out of the way of the app beneath. The shell
surface's `minimized` state cannot do that for an overlay: the compositor
sends no `state_changed` for one, SAM pauses it (`pause`, `keepAlive`), and a
`set_state(fullscreen)` on the next Home press's `relaunch` - which is how the
webosbrew SDL fork raises a window - never brings it back, leaving whatever
is beneath, or black. Unmapping the surface (the EGL surface destroyed, a
null buffer attached and committed) hides it and hands the remote to the app
beneath; on `relaunch` the EGL surface is made again and the next frame maps
it. The GL context stays, and every texture with it. The app beneath keeps
playing throughout (checked over YouTube's preview, 31 October 2026).

## What it asks the TV

All one-shot Luna calls, through `jf_luna_call` (libhelpers, on the platform
layer's GLib thread), parsed in `src/tv/tv.c`:

| for | call | answer used |
|---|---|---|
| apps | `com.webos.applicationManager/listLaunchPoints` | `launchPoints[]`: `id`, `title`, `largeIcon`/`icon` (absolute PNG paths); skips `lptype: bookmark`, `visible: false`, the HDMI apps and Home itself |
| inputs | `com.webos.service.eim/getAllInputStatus` | `devices[]` with `id` `HDMI_n`: `label`, `appId`, `hdmiPlugIn` (some TVs report `connected: true` for every port), the `SIMPLINK` entry of `subList` for the CEC device name |
| open | `com.webos.applicationManager/launch` | `{id}`; an input is its app, `com.webos.app.hdmiN` |
| overlays | `com.webos.applicationManager/getAppInfo` per candidate | the first id the TV has: input picker `com.webos.app.quickinputpicker` (webOS 11) or `com.webos.app.inputcommon` (webOS 9); quick settings `com.webos.app.quicksettings`; Settings `com.palm.app.settings`; notifications `com.webos.app.notificationcenter`; search `com.webos.app.voice` |
| settings | `com.webos.service.settings/getSystemSettings`, `getSystemSettingDesc`, `setSystemSettings` | `settings.<key>`; a choice's values in `values.arrayExt` (each with `visible`), or `values.array` on older firmware; `ui.visible`/`ui.active` hide or grey the key. Answers "does not exist" on older TVs, which have it as `com.webos.settingsservice`; the launcher switches over on that answer |
| screen off | `com.webos.service.tvpower/power/turnOffScreen` | |

Picture settings without a `dimension` read and write the picture on screen;
with the launcher over an app, that is the app's (`input: default`).

LG's screens all opened over glasshouse's replacement on a G3 (webOS 9.2):
the input picker, notifications, search, the account panel, alerts and toasts.
The quick settings tile is offered only where `getAppInfo` finds the app, as
it may not be launchable at all; the Settings key opens it regardless.

## What to check first

On the TV, in this order; each one decides the next:

1. **Permissions.** Whether the calls above answer when the launcher runs as
   `com.webos.app.home` from `/usr/palm/applications`. ls-hubd decides what a
   native client may call, and a developer app gets only public methods. The
   launcher logs every call and answer to `/tmp/home.log`.
   If they are refused, the fallback is a root helper that runs `luna-send`
   for it, as glasshouse's server does.
2. **The window type**: whether the app beneath keeps playing behind the
   ribbon, or goes black and stops. See above for the fork.
3. **The Home key while the launcher is up**: SAM should send `relaunch`,
   which brings the ribbon back to its start and asks for the lists again.
4. **sam's rescan** on this firmware: `tools/home-takeover.sh status` shows
   whether `getAppInfo` names our vendor.

## Testing without the TV

Off the TV, `src/tv/transport_mock.c` stands in for the bus: a TV in the
process that answers in the same payloads, keeps what is written to it, and
can refuse a call or answer only under the older settings name. The desktop
build runs against it, and so do the launcher's tests:

```sh
./build-host/src/home                         # the mock TV, in a window
SDL_VIDEODRIVER=offscreen UI_SCRIPT=rrrrrrro UI_CAPTURE=/tmp/home.ppm ./build-host/src/home
ctest --preset host -R home                   # tests/home_tests.c
```

The frame is transparent, so a capture shows black where the picture would be.
