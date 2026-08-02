#!/usr/bin/env bash
# LDMP installer: builds the binary, puts it on your PATH, and adds a
# `music` alias to your shell config so you can just type `music` to
# launch it (defaults to ~/Music, same as running `ldmp` with no args).
#
# Usage: ./install.sh

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

echo "==> Building ldmp"
make

INSTALL_DIR="$HOME/.local/bin"
mkdir -p "$INSTALL_DIR"
cp -f ldmp "$INSTALL_DIR/ldmp"
chmod +x "$INSTALL_DIR/ldmp"
echo "==> Installed to $INSTALL_DIR/ldmp"

# Pick the shell config file to edit
case "${SHELL:-}" in
    */zsh)  RC_FILE="$HOME/.zshrc" ;;
    */bash) RC_FILE="$HOME/.bashrc" ;;
    *)      RC_FILE="$HOME/.profile" ;;
esac
touch "$RC_FILE"

# Make sure ~/.local/bin is on PATH
if ! grep -qF '.local/bin' "$RC_FILE"; then
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> "$RC_FILE"
    echo "==> Added ~/.local/bin to PATH in $RC_FILE"
fi

# Add the `music` alias (idempotent)
if ! grep -qF "alias music=" "$RC_FILE"; then
    echo 'alias music="ldmp"' >> "$RC_FILE"
    echo "==> Added 'music' alias to $RC_FILE"
else
    echo "==> 'music' alias already present in $RC_FILE, leaving it as-is"
fi

echo
echo "Done. Restart your terminal (or run: source $RC_FILE), then just type:"
echo "  music            # opens ~/Music"
echo "  music ~/Podcasts # or any other folder"
