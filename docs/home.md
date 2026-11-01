# Home: replacing LG's launcher

`home` is a Home screen in place of LG's: a bar of app tiles along the bottom
of the screen, as LG's Home drew it before webOS 6, laid over whatever is
playing, with the HDMI inputs under it. It does not handle the Settings key or
settings at all: that key opens LG's quick settings, another app.

It is an ordinary developer app. What makes it Home is
[Home Chooser](https://github.com/webosbrew/home-chooser), a homebrew app that
makes the Home key open the launcher the owner picks, keeps that across
restarts, and puts LG's Home back if the launcher or Home Chooser is removed.

```sh
cmake --build build --target home-install
```

Home Chooser has to be installed and opened once **before** the launcher: the
TV refuses to install an app naming a permission nobody declares yet (`Not
allowed permission is used`). Then open Home in LG's menu. If another app is
Home, it opens Home Chooser asking the owner to choose it, and gets out of the
way. To go back, open Home Chooser and choose LG Home; removing Home Chooser or
the launcher does the same by itself.

## What Home Chooser does for it

On webOS 6 to 9:

- The Home key: a copy of the TV's key filter launching the launcher instead,
  selected through configd until the next restart.
- SAM opening `com.webos.app.home` for any other reason: a gate, mounted over
  `/usr/bin/com.webos.app.home`, decides. With `defaultWindowType: overlay`:
  nothing in front opens the last input, and something in front opens the
  launcher. The launcher gets only its id then, not why.
- A watcher redoes all of it at every start, as a boot hook of Home Chooser's.

So the launcher has no part in the TV coming on or an app closing. Closing an
app goes back to the last input, not to the menu.

Home Chooser refuses a key filter whose shape it does not know, and leaves
LG's Home on the key. Its list of checked firmware has 9.2.1 and 9.2.4, not
our 9.2.2.

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
through and around the ribbon (checked over YouTube on 31 October 2026, then
still running as LG's Home). It
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

A developer app has the bus groups `public` and `ares.webos.cli`
(`client-permissions.d`, `devmode_certificate.json`), and what those allow was
read from `/usr/share/luna-service2/api-permissions.d` on webOS 9.2.2. `luna-send
-a <app>` does not show it: as root it answers an id that does not exist too.
What they do not allow goes through Home Chooser's root service, whose methods
need `requiredPermissions` naming `org.webosbrew.homechooser.applications`.

All one-shot Luna calls, through `jf_luna_call`, parsed in `src/tv/tv.c`:

| for | call | answer used |
|---|---|---|
| apps | Home Chooser `listApps {subscribe: true}` (`listApps` itself is private), asked once while it keeps telling of changes, and at every opening where it does not | `apps[]`: `id`, `title`, `folderPath`, `icon`, `largeIcon`, `version`, `iconColor`, `removable`, `visible`; skips `visible: false`, the HDMI apps and Home itself |
| icons | The app's folder where the jail can read it; else Home Chooser `getAppIcon {id, size}`, kept in `data/icons/<id>-<version>` | `data`, base64. On webOS 9.2.2 the jail sees `/media/developer/apps` but not `/usr/palm`, `/media/system/apps` or `/mnt/otncabi`: 12 of 21 visible apps. The large icon, then the small one |
| inputs | `com.webos.service.eim/getAllInputStatus` (public) | `devices[]` with `id` `HDMI_n`: `label`, `appId`, `hdmiPlugIn` (some TVs report `connected: true` for every port), the `SIMPLINK` entry of `subList` for the CEC device name |
| front | Home Chooser `getForegroundApp` (`getForegroundAppInfo` is private) | `appId`: the app beneath the launcher, which as an overlay is not the foreground app itself; the launcher starts on its tile, or its input's card |
| open | `com.webos.applicationManager/launch` (public) | `{id}`; an input is its app, `com.webos.app.hdmiN` |
| remove | Home Chooser `uninstallApp {id}` | `pending`: it asks the owner itself, after the launcher's own question. If they agree the app leaves the subscribed list |
| Home Chooser | Home Chooser `getInfo` | `apiVersion`, `root`, `permitted`, `isSelectedHome`, in that order; each but the last is a line on screen |

`iconColor` and `subscribe` in `listApps`, and native callers at all, are not in
Home Chooser's API yet; without the first tiles are grey, and without the second
the list is asked for at every opening. `eim/getAllInputStatus` answers
`subscribe: true` without `subscribed`, so the inputs are asked for at every
opening too, as is what is in front. `com.webos.service.recent/listRecents`, the frames LG's Home shows on
an input's card, is private and Home Chooser does not offer it, so the cards
show a glyph. On webOS 9.2.2 its paths for the inputs were empty anyway.

The order of the apps and which are hidden are the launcher's own
(`src/home/layout.c`), one line per app (`show <id>` / `hide <id>`) in
`data/layout.txt` in the app's folder, its working directory, which a reinstall
leaves in place (seen on webOS 9.2.2). `HOME_LAYOUT` names another file. With no layout yet the apps are
in `listApps`'s order, by id.

## How long it lives

Started by the first Home press, it stays running: Home brings it back, and
hiding it unmaps the window, keeping the GL context, textures and icons. Nothing
freezes it; it sleeps until an event comes. It held about 32 MB on webOS 9.2.2.

It is not kept alive by anything either. A `_WEBOS_WINDOW_TYPE_RESTRICTED` window
such as LiveTV's `com.webos.app.channelsetting` closes it (`close`, reason
`windowPolicy`), and once it died hidden with no cause in any log. The next Home
press starts it afresh. `/tmp/home.log` is this run's; the last run's is
`/tmp/home.log.1`.

## The bar

- The tiles, parallelograms side by side, stand over the picture. The focused one grows up along its slanted sides, its colour still
  reaching the bottom edge; the one losing the focus sinks back, both eased.
  Tiles carry no names. A tile without an icon shows its first letter in
  black or white, whichever contrasts more with the tile (WCAG relative
  luminance). The bar starts at the screen's side,
  where the first tile's bottom left corner is.
- Behind the bar, always, is one fade, from nothing at the top of the apps to
  solid at their foot, and solid below that, where the inputs come up. It moves
  with the bar and nothing about it depends on how far that has risen. A fade is only true on a quad that
  ends with it: `ui.slang` clamps it at the corners before interpolating, so on
  a longer quad it runs past the width asked for.
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
  for LG's Home, and the launcher leaves it: the arrow keys hide it, and moving
  the remote brings it back.
- Resting the pointer in the outer 120 px at either side of the bar scrolls
  it that way, steadily, faster the nearer the edge: up to 1400 px a second.
- Opening, nothing eases: the first frames, through the focus going to what is
  in front, are drawn at their end state, until a key or the pointer moves.
- An app opened over another minimises the one beneath (SDL: `FOCUS_LOST`, then
  `MINIMIZED`); the overlay launcher only takes its focus. SAM sends a native
  app no `pause` for either. Jellyfin pauses playback when its window is
  minimised, so it plays on under the menu but stops for another app.
- Down raises the bar and focuses the inputs; Up goes back to the app the
  focus left, and Down again to the input it left.
- The pencil after the apps opens the edit mode, on the app beside it. Select
  picks an app up, with
  arrows either side of it; Left and Right carry it; select puts it down. The
  pointer does the same: a click picks an app up, it follows the pointer with
  the focus staying on it, at either side scrolling the bar, and a click anywhere
  puts it down; or it is dragged, and letting go puts it down. A pick-up that
  moves it to no other place is not a drag, as the remote shakes when clicked. Up
  reaches the X over it, which asks whether to hide or uninstall it; an app
  that is not `removable` can only be hidden. The hidden apps follow the shown
  ones, greyed out; selecting one shows it again and picks it up. Back saves
  and leaves the edit mode, as does the Home key.

## What to check first

On the TV, in this order; each one decides the next:

1. **Whether Home Chooser takes the launcher as a caller.** It requires
   an application id, which a native app's connection does not carry on webOS
   9.2.2: it answered `forbidden`, and `LSRegisterApplicationService` is refused
   (`Invalid permissions`), as the role goes by the executable and names no app.
   Home Chooser needs to take the bus name for those. `getInfo`'s answer is on
   the bar's line and in `/tmp/home.log`.
2. **Whether the key filter is accepted** on 9.2.2: Home Chooser's status shows
   `patch=possible`.
3. **The window type under the developer id**: that the app beneath keeps
   playing, as it did as LG's Home.
4. **The Home key's reason**: `relaunch` with `hotKey` is what toggles the bar.
5. **The TV coming on**: Home is opened twice, and if the input is in front by
   the second, the gate opens the launcher over it.
6. **Whether a hidden developer app stays alive**, which LG's Home was kept for.

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
