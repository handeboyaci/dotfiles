# Waybar migration status

The Sway status bar has been migrated from py3status to a custom Waybar 0.15.0 build.

## Completed

- Sway launches `/home/sselcuk/.local/bin/waybar` through `swaybar_command`.
- Waybar configuration and styling live in `~/.dotfiles/waybar`.
- Native/custom modules cover workspaces, scratchpad, Quiet notifications, Google
  Calendar, MPRIS media control, PipeWire/PulseAudio volume, prayer times,
  battery status, swayidle control, and the system tray.
- `waybar-clock` provides the interactive date, timezone, and calendar display.
- `pixelbuds-pause` runs separately as a user service for BlueZ wear detection.
- The setup notes now link `~/.config/waybar` with the other dotfiles.

## Remaining deployment steps

- Install the `playerctl` package and run `playerctld daemon` in the graphical
  user session; Waybar's MPRIS module uses that proxy to follow the active player.
- Authorize the GitHub CLI `workflow` scope and push the already committed
  `custom-modules` branch to `sencer/Waybar`.
- Commit the dotfiles changes after visually checking the bar in the live Sway
  session.

## Source locations

- Custom Waybar source: `~/src/waybar`
- Waybar config: `~/.dotfiles/waybar`
- Sway config: `~/.dotfiles/sway/config`
- Helper sources: `~/.dotfiles/data`; compiled binaries: `~/.local/bin`
- Pixel Buds unit: `~/.dotfiles/systemd/user/pixelbuds-pause.service`
