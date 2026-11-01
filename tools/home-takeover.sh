#!/bin/sh
# The launcher in LG's Home's place, until the next reboot. Runs on the TV, as
# root:
#
#   ssh root@tv sh -s take    < tools/home-takeover.sh
#   ssh root@tv sh -s restore < tools/home-takeover.sh
#   ssh root@tv sh -s status  < tools/home-takeover.sh
#
# The launcher carries this script and runs it itself: opened from LG's menu,
# through Homebrew Channel's root service, as `take open`; and at every start
# when it is asked to, through Homebrew Channel's boot hooks, as `boot`. Off
# again, from anywhere:
#
#   ssh root@tv sh -s disable-boot < tools/home-takeover.sh
#
# The launcher must be installed first (cmake --build build --target
# home-install). Nothing on the root filesystem changes: a copy of it, under
# LG's Home's id, is bind-mounted over /usr/palm/applications/
# com.webos.app.home, and everything the TV opens as Home - the Home key, Back
# out of the last app, waking from standby - opens it. Both the copy and the
# mount live in /tmp, so a reboot puts LG's Home back whatever else happens.
#
# The steps and their order are glasshouse's (lib/home-takeover.js and
# lib/samrescan.js), measured on webOS 7 to 11: sam reads an appinfo.json only
# when it scans, so it is made to read this one again, and only then is the
# Home that is running stopped.
set -u

LG_HOME=com.webos.app.home
APP_DIR=/usr/palm/applications/$LG_HOME
# ls-hubd gives the name com.webos.app.home, and LG's Home's privileged role,
# only to the process running this executable (/usr/share/ls2/roles/*/
# com.webos.app.home.json), so the launcher is mounted over it too. Elsewhere
# it cannot register with sam, which shows its launch spinner forever, and no
# call it makes is answered.
LG_EXE=/usr/bin/$LG_HOME
OURS=dev.hookedbehemoth.home
INSTALLED=/media/developer/apps/usr/palm/applications/$OURS
STAGE=/tmp/home-takeover
BLOCK_SCHEMA=/etc/palm/schemas/sam/applicationManager.blockSystemApp.schema
# Not an app: blocking it blocks nothing, and makes sam rescan. A new one
# every time, so that the list always changes.
NUDGE_ID=dev.hookedbehemoth.home.rescan
# Run at boot by Homebrew Channel while it is there (home-boot-hook).
BOOT_HOOK=/var/lib/webosbrew/init.d/60-home-takeover

say() { echo "home-takeover: $*" >&2; }

# luna-send answers nothing with stdin at its end over SSH; held open, it does.
luna() { sleep 3 | luna-send -n 1 -w 3000 "luna://$1" "$2" 2>/dev/null; }

# Said on the screen too: run from the launcher, there is no terminal.
fail() {
  say "$*"
  sleep 3 | luna-send -n 1 -w 3000 -a "$LG_HOME" \
    luna://com.webos.notification/createToast \
    "{\"sourceId\":\"$LG_HOME\",\"message\":\"$*\"}" >/dev/null 2>&1
}

same_file() { [ "$(stat -L -c %d:%i "$1" 2>/dev/null)" = "$(stat -L -c %d:%i "$2" 2>/dev/null)" ]; }

mounted() { [ -d "$STAGE" ] && same_file "$APP_DIR" "$STAGE"; }
exe_mounted() { [ -f "$STAGE/home" ] && same_file "$LG_EXE" "$STAGE/home"; }

# Every mount over `path`, by what the kernel lists rather than by the stage
# it came from: one left by a run that went wrong outlives its stage, and
# stacks under the next.
unmount_all() {
  for _ in 1 2 3 4 5 6 7 8; do
    grep -q " $1 " /proc/mounts || return 0
    umount "$1" 2>/dev/null || umount -l "$1"
  done
  ! grep -q " $1 " /proc/mounts
}

# Processes running LG's executable path, and whether each is the launcher
# under it or LG's own: the path is the same, the file is not.
home_pids() {
  ours=$1
  for proc in /proc/[0-9]*; do
    exe=$(readlink "$proc/exe" 2>/dev/null) || continue
    case "$exe" in "$LG_EXE"* | "$APP_DIR"/home | "$STAGE"/home) ;; *) continue ;; esac
    if same_file "$proc/exe" "$STAGE/home"; then
      [ "$ours" = ours ] && echo "${proc#/proc/}"
    else
      [ "$ours" = lgs ] && echo "${proc#/proc/}"
    fi
  done
  # webOS 10 and 11 run LG's Home in the Flutter runner, given its id.
  if [ "$ours" = lgs ]; then
    for proc in /proc/[0-9]*; do
      case "$(tr '\0' ' ' 2>/dev/null <"$proc/cmdline")" in
      *flutter-client*"-i $LG_HOME"*) echo "${proc#/proc/}" ;;
      esac
    done
  fi
}

# What sam has under LG's Home's id. Its paths are the mount point's either
# way, so the manifests are told apart by their vendor.
sam_info() { luna com.webos.applicationManager/getAppInfo "{\"id\":\"$LG_HOME\"}"; }

sam_has_ours() {
  info=$(sam_info)
  echo "$info" | grep -q '"vendor": *"webos-native"' &&
    echo "$info" | grep -q "\"main\": *\"file://$LG_EXE\""
}
sam_has_lgs() { sam_info | grep -q '"returnValue": *true' && ! sam_has_ours; }

# Polls until `check` holds, for up to eight seconds.
wait_for() {
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do
    "$1" && return 0
    sleep 0.5
  done
  return 1
}

# webOS 10 and 11: blocking a system app and releasing it makes sam drop and
# read that one app again. webOS 11 refuses the block without
# forceBlockSystemApp, which only its schema names.
#
# webOS 9.2.2 has the block but not the release ("Unknown method"), and a
# block there lasts until sam restarts: Home opens nothing until a reboot. So
# the release is asked first, for an id that is not blocked - which webOS 10
# answers true and changes nothing - and only a TV that has it is blocked.
can_release() {
  luna com.webos.applicationManager/releaseBlockSystemApp \
    "{\"id\":\"$NUDGE_ID\"}" | grep -q '"returnValue": *true'
}

reread_app() {
  can_release || return 1
  force=""
  grep -q forceBlockSystemApp "$BLOCK_SCHEMA" 2>/dev/null &&
    force=',"forceBlockSystemApp":false'
  luna com.webos.applicationManager/blockSystemApp \
    "{\"id\":\"$LG_HOME\"$force}" | grep -q '"returnValue": *true' || return 1
  # An app left blocked cannot launch, so the release is tried until it holds.
  for _ in 1 2 3; do
    luna com.webos.applicationManager/releaseBlockSystemApp \
      "{\"id\":\"$LG_HOME\"}" | grep -q '"returnValue": *true' && return 0
  done
  say "LG's Home may still be blocked: release it with releaseBlockSystemApp"
  return 1
}

# webOS 7 to 9: sam rescans every app when profile.blockedAppList changes.
# The list gets an id no app has and then exactly what it had. configd keeps
# what it was given as a user value across reboots and has no way to forget
# one. Where it held none, sam blocks the list LG's server sent for the
# country, so that list is what is written back, to leave sam blocking what it
# blocked.
COUNTRY_LISTS=/var/preferences/com.webos.applicationManager/blockedAppList

nudge() {
  current=$(luna com.webos.service.config/getConfigs \
    '{"configNames":["profile.blockedAppList"]}' |
    tr -d '\n' | sed -n 's/.*"profile.blockedAppList": *\(\[[^]]*\]\).*/\1/p')
  # Without an id an earlier run left in it, when one stopped half way.
  current=$(echo "$current" | sed "s/\"$NUDGE_ID[^\"]*\" *,\{0,1\} *//g; s/, *\]/]/")
  if [ -z "$current" ] || [ "$current" = "[]" ]; then
    current=$(cat "$COUNTRY_LISTS"/*.json 2>/dev/null | head -n 1 |
      sed -n 's/.*"blocked_applist": *\(\[[^]]*\]\).*/\1/p')
    [ -n "$current" ] || current='[]'
  fi
  nudge_id="$NUDGE_ID.$(date +%s)"
  with=$(echo "$current" | sed "s/^\[ *\]$/[\"$nudge_id\"]/;t
    s/^\[\(..*\)\]$/[\1,\"$nudge_id\"]/")
  luna com.webos.service.config/setConfigs \
    "{\"configs\":{\"profile.blockedAppList\":$with}}" >/dev/null || return 1
  sleep 1.5
  luna com.webos.service.config/setConfigs \
    "{\"configs\":{\"profile.blockedAppList\":$current}}" >/dev/null
}

# sam reading LG's Home's id again until `check` holds: the nudge first, as
# glasshouse does everywhere but webOS 10 and 11, where sam ignores it.
reread() {
  check=$1
  if nudge && wait_for "$check"; then return 0; fi
  if reread_app && wait_for "$check"; then return 0; fi
  return 1
}

stop_lg_home() {
  for pid in $(home_pids lgs); do
    kill "$pid" 2>/dev/null && say "stopped LG's Home ($pid)"
  done
  # webOS 11 starts it again in the background to have it ready.
  luna com.webos.service.preloadmanager/setPreloadPolicy \
    "{\"application\":{\"id\":\"$LG_HOME\",\"isEnabled\":false}}" >/dev/null
}

stop_ours() {
  for pid in "$@"; do kill "$pid" 2>/dev/null; done
}

stage() {
  [ -x "$INSTALLED/home" ] || {
    fail "Install the launcher first: there is no $INSTALLED/home"
    return 1
  }
  # The files once; the manifest every time, through a mount if there is one.
  if ! mounted && ! exe_mounted; then
    unmount_all "$LG_EXE" && unmount_all "$APP_DIR" || return 1
    rm -rf "$STAGE" && mkdir -p "$STAGE" && cp -a "$INSTALLED/." "$STAGE/" ||
      return 1
  fi
  # LG's Home's id, so everything that opens Home opens this; an overlay
  # window, as LG's Settings and Search are, laid over the app beneath rather
  # than sending it to the background; and
  # the home category, without which webOS 10's Home key opens nothing.
  cat >"$STAGE/appinfo.json" <<EOF
{
  "id": "$LG_HOME",
  "version": "1.0.0",
  "vendor": "webos-native",
  "type": "native",
  "main": "file://$LG_EXE",
  "title": "Home",
  "icon": "icon.png",
  "visible": false,
  "appCategory": ["home"],
  "defaultWindowType": "overlay",
  "nativeLifeCycleInterfaceVersion": 2,
  "noSplashOnLaunch": true
}
EOF
}

# With `open`, the launcher is opened once it is in place; without, only if
# LG's Home was in front, which stopping it leaves empty.
take() {
  open=${1:-}
  if mounted && exe_mounted; then
    say "already in place"
    [ "$open" = open ] && open_home
    return 0
  fi
  stage || exit 1
  mounted || mount --bind "$STAGE" "$APP_DIR"
  exe_mounted || mount --bind "$STAGE/home" "$LG_EXE"
  mounted && exe_mounted || {
    fail "The launcher could not take LG's Home's place: the mounts did not take"
    restore
    exit 1
  }
  if ! reread sam_has_ours; then
    fail "The launcher could not take LG's Home's place: the TV did not take it as Home"
    restore
    exit 1
  fi
  luna com.webos.applicationManager/getForegroundAppInfo '{}' |
    grep -q "\"appId\": *\"$LG_HOME\"" && open=open
  stop_lg_home
  # A launcher an earlier take left running, started from another path: sam
  # starts it again from the manifest it has now. Not before the reread, or
  # it starts again from the old one.
  stop_ours $(home_pids ours)
  say "in place until the next reboot"
  [ "$open" = open ] && open_home
  return 0
}

open_home() {
  luna com.webos.applicationManager/launch "{\"id\":\"$LG_HOME\"}" >/dev/null
}

# At boot, sam is not up yet when Homebrew Channel runs its hooks: waited
# for, up to two minutes, before taking its place.
boot() {
  for _ in $(seq 60); do
    sam_info | grep -q '"returnValue": *true' && break
    sleep 2
  done
  take
}

disable_boot() {
  rm -f "$BOOT_HOOK"
  say "not at the next start"
}

restore() {
  # Found while the file can still be told apart, stopped last.
  ours=$(home_pids ours)
  unmount_all "$LG_EXE" || say "$LG_EXE is still mounted over"
  unmount_all "$APP_DIR" || say "$APP_DIR is still mounted over"
  reread sam_has_lgs ||
    say "sam has not reread LG's Home; a reboot puts it back"
  luna com.webos.service.preloadmanager/setPreloadPolicy \
    "{\"application\":{\"id\":\"$LG_HOME\",\"isEnabled\":true}}" >/dev/null
  # Last, once sam has LG's manifest. A Home opened while this ran - the Home
  # key pressed while it waited - started with LG's executable but the
  # launcher's folder still mounted, and draws nothing: stopped with the
  # launcher, and LG's Home opened afresh.
  stop_ours $ours
  for pid in $(home_pids lgs); do kill "$pid" 2>/dev/null; done
  rm -rf "$STAGE"
  sleep 1
  luna com.webos.applicationManager/launch "{\"id\":\"$LG_HOME\"}" >/dev/null
  say "LG's Home is back"
}

status() {
  if mounted; then echo "mounted over $APP_DIR"; else echo "not mounted"; fi
  if exe_mounted; then echo "mounted over $LG_EXE"; fi
  if [ -e "$BOOT_HOOK" ]; then echo "taking LG's Home's place at every start"; fi
  echo "launcher: $(home_pids ours)  LG's Home: $(home_pids lgs)"
  sam_info
}

case "${1:-}" in
take) take "${2:-}" ;;
boot) boot ;;
restore) restore ;;
disable-boot) disable_boot ;;
status) status ;;
*)
  echo "usage: home-takeover.sh take [open]|boot|restore|disable-boot|status" >&2
  exit 2
  ;;
esac
