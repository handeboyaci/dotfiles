#!/usr/bin/env bash
set -e

DOTFILES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Installing packages..."
sudo apt-get update -qq
sudo apt-get install -y tmux zsh neovim fzf ripgrep fd-find

# fd is shipped as fdfind on Debian/Ubuntu
sudo ln -sf /usr/bin/fdfind /usr/local/bin/fd

# Set zsh as default shell
sudo chsh -s /usr/bin/zsh "$USER"

echo "==> Symlinking dotfiles from $DOTFILES_DIR..."

ln -sf "$DOTFILES_DIR/tmux.conf"    "$HOME/.tmux.conf"
ln -sf "$DOTFILES_DIR/bashrc"       "$HOME/.bashrc"
ln -sf "$DOTFILES_DIR/gitconfig"    "$HOME/.gitconfig"
ln -sf "$DOTFILES_DIR/inputrc"      "$HOME/.inputrc"
ln -sf "$DOTFILES_DIR/zsh/zshenv"   "$HOME/.zshenv"

mkdir -p "$HOME/.config"
ln -sf "$DOTFILES_DIR/nvim"         "$HOME/.config/nvim"

mkdir -p "$HOME/.config/tmux"
ln -sf "$DOTFILES_DIR/tmux"         "$HOME/.config/tmux/plugins"

nvim --headless -c ':TSUninstall all | q'
nvim --headless -c ':TSInstallSync all | q'
echo "==> Done! All dotfiles installed."
echo "Open nvim and run\n:MasonInstall basedpyright bash-language-server lua-language-server bandit ruff shellcheck shfmt vim-language-server vint'
