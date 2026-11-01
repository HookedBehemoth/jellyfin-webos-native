# Home: replacing LG's launcher

`home` is a Home screen in place of LG's: a bar of app tiles along the
bottom of the screen, as LG's Home drew it before webOS 6, laid over whatever
is playing, with the HDMI inputs under it. It does not handle the Settings
key or settings at all: that key opens LG's quick settings, another app. It is
not a port of
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

### On the TV, without ssh

The ipk carries `home-takeover.sh` and `home-boot-hook`.

- **Opening "Home" from LG's menu** takes the place for this start. Opened
  under its own id, the launcher is a developer app without root, so it asks
  Homebrew Channel's root service
  (`luna://org.webosbrew.hbchannel.service/exec`, open to any caller) to run
  `home-takeover.sh take open` in the background, and quits. The script opens
  the new Home once it is in place, and says on screen (a toast) when it
  could not. Without Homebrew Channel, a toast says that is what is missing.
- **The gear tile** at the end of the bar (options): *At every start* links
  `home-boot-hook` as `/var/lib/webosbrew/init.d/60-home-takeover`. Homebrew
  Channel runs everything there at boot; the hook returns at once and leaves
  the script to wait for sam (up to two minutes) and take the place. In LG's
  Home's place the launcher runs as root, so it makes and removes the link
  itself. *Restart the TV* restarts it
  (`com.webos.service.sleep/shutdown/machineReboot`), leaving the link as it
  is: with *At every start* off, that gives LG's Home its place back for
  certain. Undoing the takeover in place (`restore`) is
  left to a shell.
- **Recovery** with *At every start* on: remove the link from a shell
  (`home-takeover.sh disable-boot`, or `rm` it) and reboot or `restore`. With
  it off, a reboot is always enough. Homebrew Channel's own failsafe also
  skips every boot hook on the start after one that a reboot cut short.

Glasshouse does the same with a setting its server reads at every start and
a boot hook of its own (`50-tvweb`), applied without asking; here nothing
runs at boot until the option is turned on.

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
   - The id is a new one every time (`dev.hookedbehemoth.home.rescan.<time>`),
     and any left in the list is taken out when it is written back. A run that
     stopped half way once left the list holding just the id; adding the same
     id again changed nothing sam counts, and no takeover took after that.
4. Only then stop LG's running Home (its process, or on webOS 10/11 the
   `flutter-client -i com.webos.app.home` runner). Changing the manifest's type
   under a running instance leaves sam unable to close or replace it.
5. webOS 11's preload manager starts LG's Home again in the background;
   `preloadmanager/setPreloadPolicy` with `isEnabled: false` stops that, and
   `restore` turns it back on.

`restore` unmounts, rereads, stops our process, and opens LG's Home. It
unmounts whatever `/proc/mounts` lists over the two paths, not only what
matches the stage: a mount a failed run left behind outlives its stage, and
LG's Home started over the empty folder it leaves draws nothing (a black
screen). For the same reason a Home that started while `restore` ran - the
Home key pressed while it waited for sam, after the executable was LG's again
but before the folder was - is stopped, and LG's Home opened afresh.

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
| tiles | the same launch points | `iconColor` (`#rrggbb`, the tile's colour, as LG's Home paints it; often `""`), `removable` (false for LG's own apps) |
| previews | `com.webos.service.recent/listRecents` | `recentsAppList[]`: `appId`, `cardSnapShotFilePath`, the last frame the compositor kept of an app, which LG's Home shows on an input's card |
| front | `com.webos.applicationManager/getForegroundAppInfo` | `appId`: the app beneath the launcher, which as an overlay is not the foreground app itself (checked on webOS 9.2.2); the launcher starts on its tile, or its input's card |
| open | `com.webos.applicationManager/launch` | `{id}`; an input is its app, `com.webos.app.hdmiN` |
| remove | `com.webos.appInstallService/remove` | `{id, subscribe: false}`, as LG's Home sends it; the launch points are asked again after the answer |

`com.webos.service.recent` is the compositor itself (`libWebOSCoreCompositor`
registers it), found in LG's Home QML (`Interfaces/HomeRecentInterfaces`). On
webOS 9.2.2 it answers, but only apps that supply an `imageForRecents` have a
path: the HDMI inputs' are empty there, so their cards show a glyph. How the
compositor captures one for an input was not chased further.

The order of the apps and which are hidden are the launcher's own
(`src/home/layout.c`), one line per app (`show <id>` / `hide <id>`) in
`layout.txt` beside the installed app,
`/media/developer/apps/usr/palm/applications/dev.hookedbehemoth.home/`, so
LG's Home keeps its own order (`moveLaunchPoint`) when it has its place back.
A reinstall starts it afresh. `HOME_LAYOUT` names another file.

## Coming on

At power-on, from standby or a restart, bootManager opens Home (reason
`boot`, `parameters: {boot: true}`), and a second later sam's
`last_input_handler` opens it again (reason `undefined`, mode `last_input`).
LG's Home opens the last input then (`general.lastInputApp` in
`com.webos.settingsservice`, `com.webos.app.livetv` on ours) and stays out of
sight. The launcher does the same for both, within 15 s of the first; showing
itself there left nothing in front: a black screen once it was put away. The
same applies when it is started rather than relaunched by the TV coming on.

## The bar

- No background: the tiles, parallelograms side by side, stand over the
  picture. The focused one grows up along its slanted sides, its colour still
  reaching the bottom edge; the one losing the focus sinks back, both eased.
  Tiles carry no names. A tile without an icon shows its first letter in
  black or white, whichever contrasts more with the tile (WCAG relative
  luminance). The bar starts at the screen's side,
  where the first tile's bottom left corner is.
- With the inputs up, they sit on a solid background reaching up to the top
  of the apps.
- Coming up, the launcher starts on what is in front beneath it, unless a key
  is pressed before the TV says what that is.
- Up from the apps puts the launcher out of the way, as Back does.
- The tiles are 7.5% of the screen wide and 18.5% high, leaning 17%. A
  slanted fill is clipped before the vertex shader shears it, so the renderer
  lets it run past the left of its clip by the shear's reach; otherwise a tile
  half off the left edge loses the triangle the shear brings back into view.
- The Magic Remote's pointer: `wl_webos_input_manager` (requests
  `set_cursor_visibility(u)` and `get_webos_seat(no)`, checked on webOS 9.2.2)
  reports its visibility as soon as it is bound and whenever it changes. The
  compositor brings the pointer up whenever the launcher is shown, as it does
  for LG's Home, and the launcher leaves it: every key hides it, as LG's SDL
  does, and moving the remote brings it back.
- Resting the pointer in the outer 120 px at either side of the bar scrolls
  it that way, steadily, faster the nearer the edge: up to 1400 px a second.
- Opening, nothing eases: the first frames, through the focus going to what is
  in front, are drawn at their end state, until a key or the pointer moves.
- Closing an app (Exit, or `ares-launch --close`), SAM relaunches Home twice
  (reason `undefined`, nothing in front). The launcher comes up over black,
  showing nothing but the menu, until it is put away: over black, or over
  `/media/developer/home-background.png` when there is one (a PNG, cropped to
  fill the screen; `HOME_BACKGROUND` names another). Being an overlay is no
  bar to that: the frame is the launcher's to draw, and it covers what is
  beneath wherever it is opaque. A raise while it is up
  and focused does not remap it, which flickered.
- The compositor may also bring the last input up when an app closes:
  `StarfishFullscreenController.qml`'s closing policy, under its read-only
  setting `fullscreen.launchLastInputAppAfterClosing` (true), launches the app
  the `general.lastInputApp` setting names when the closing window asks for it.
  Coming up full screen, any app closes every overlay
  (`StarfishViewStateController.qml`), and SAM sends the launcher a `pause`.
  A pause within 3 s of such a relaunch, before any key, brings the launcher
  back over it, once. An overlay can be kept up with
  `com.webos.surfacemanager/setPinOverlayView {appId, overlayId}`, but that is
  in the `surfaces.control` group, which LG Home's role does not have:
  "Denied method call" on webOS 9.2.2. Neither Exit nor `--close` brought an
  input up in testing; when the remote's Back closes an app it may.
- An app opened over another minimises the one beneath (SDL: `FOCUS_LOST`, then
  `MINIMIZED`); the overlay launcher only takes its focus. SAM sends a native
  app no `pause` for either. Jellyfin pauses playback when its window is
  minimised, so it plays on under the menu but stops for another app.
- Down raises the bar and focuses the inputs, over a background that is solid
  below the apps and fades out to their top; Up goes back to the app the
  focus left, and Down again to the input it left.
- The pencil after the apps opens the edit mode, on the app beside it. Select
  picks an app up, with
  arrows either side of it; Left and Right carry it; select puts it down. Up
  reaches the X over it, which asks whether to hide or uninstall it; an app
  that is not `removable` can only be hidden. The hidden apps follow the shown
  ones, greyed out; selecting one shows it again and picks it up. Back saves
  and leaves the edit mode, as does the Home key.

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
   which brings the bar back to its first screen and asks for the lists again.
4. **sam's rescan** on this firmware: `tools/home-takeover.sh status` shows
   whether `getAppInfo` names our vendor.

## Testing without the TV

Off the TV, `src/tv/transport_mock.c` stands in for the bus: a TV in the
process that answers in the same payloads, forgets an app removed from it,
and can refuse a call. The desktop
build runs against it, and so do the launcher's tests:

```sh
./build-host/src/home                         # the mock TV, in a window
SDL_VIDEODRIVER=offscreen UI_SCRIPT=rrro UI_CAPTURE=/tmp/home.ppm ./build-host/src/home
ctest --preset host -R home                   # tests/home_tests.c
```

The frame is transparent, so a capture shows black where the picture would be.
