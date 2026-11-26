#!/usr/bin/env bash
# Configure iTerm2 to match ghostty/config via a Dynamic Profile and defaults.
set -euo pipefail

DOTFILES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE_SRC="$DOTFILES_DIR/iterm2/dotfiles.json"
PROFILE_DIR="$HOME/Library/Application Support/iTerm2/DynamicProfiles"
PROFILE_DST="$PROFILE_DIR/dotfiles.json"
PROFILE_GUID="8D4C9B3A-5F2E-4A1D-9C6B-7E8F0A1B2C3D"

mkdir -p "$PROFILE_DIR"
if [ -L "$PROFILE_DST" ]; then
  rm "$PROFILE_DST"
elif [ -e "$PROFILE_DST" ]; then
  mv "$PROFILE_DST" "$PROFILE_DST.backup-$(date +%Y%m%d-%H%M%S)"
fi
ln -s "$PROFILE_SRC" "$PROFILE_DST"

defaults write com.googlecode.iterm2 "Default Bookmark Guid" "$PROFILE_GUID"
defaults write com.googlecode.iterm2 AllowClipboardAccess -bool true
defaults write com.googlecode.iterm2 PromptOnQuit -bool false
defaults write com.googlecode.iterm2 OnlyWhenMoreTabs -bool false
defaults write com.googlecode.iterm2 TerminalMargin -int 0
defaults write com.googlecode.iterm2 TerminalMarginTopBottom -int 0

echo "iTerm2 configured (Dynamic Profile linked and defaults applied)."
echo "Note: Restart iTerm2 if it is already running so defaults take effect."
