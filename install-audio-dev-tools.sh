#!/usr/bin/env bash
set -euo pipefail

echo "==> Checking macOS"
if [[ "$(uname)" != "Darwin" ]]; then
  echo "This script is intended for macOS."
  exit 1
fi

echo "==> Checking Xcode Command Line Tools"
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Installing Xcode Command Line Tools..."
  xcode-select --install || true
  echo ""
  echo "After the installation finishes, rerun this script."
  exit 0
fi

echo "==> Checking Homebrew"
if ! command -v brew >/dev/null 2>&1; then
  echo "Installing Homebrew..."
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

  if [[ -x /opt/homebrew/bin/brew ]]; then
    eval "$(/opt/homebrew/bin/brew shellenv)"
  elif [[ -x /usr/local/bin/brew ]]; then
    eval "$(/usr/local/bin/brew shellenv)"
  fi
fi

echo "==> Updating Homebrew"
brew update

echo "==> Installing build tools"
brew install \
  git \
  cmake \
  ninja \
  pkg-config \
  ccache

echo "==> Installing optional audio/dev utilities"
brew install \
  ffmpeg \
  libsndfile \
  sox

echo "==> Creating dev folder"
mkdir -p "$HOME/dev/audio"
cd "$HOME/dev/audio"

echo "==> Cloning JUCE if missing (reference/Projucer copy; the CMake build fetches its own pinned JUCE via FetchContent)"
if [[ ! -d "$HOME/dev/audio/JUCE/.git" ]]; then
  git clone https://github.com/juce-framework/JUCE.git "$HOME/dev/audio/JUCE"
  # Pin to the latest release tag rather than an in-development HEAD.
  git -C "$HOME/dev/audio/JUCE" fetch --tags --quiet || true
  latest_tag="$(git -C "$HOME/dev/audio/JUCE" describe --tags "$(git -C "$HOME/dev/audio/JUCE" rev-list --tags --max-count=1)" 2>/dev/null || true)"
  if [[ -n "$latest_tag" ]]; then
    git -C "$HOME/dev/audio/JUCE" checkout --quiet "$latest_tag"
    echo "JUCE pinned to $latest_tag"
  fi
else
  echo "JUCE already exists at $HOME/dev/audio/JUCE"
fi

echo "==> Checking for full Xcode"
if ! command -v xcodebuild >/dev/null 2>&1; then
  echo "xcodebuild not found. Install full Xcode from the App Store for AU builds and signing."
else
  echo "Xcode tools available:"
  xcodebuild -version || true
fi

echo ""
echo "Done."
echo ""
echo "Installed/checked:"
echo "- Xcode Command Line Tools"
echo "- Homebrew"
echo "- git"
echo "- cmake"
echo "- ninja"
echo "- pkg-config"
echo "- ccache"
echo "- ffmpeg"
echo "- libsndfile"
echo "- sox"
echo "- JUCE at ~/dev/audio/JUCE"
echo ""
echo "Recommended next manual installs:"
echo "- Full Xcode from the App Store (needed for AU auval + signing)"
echo "- pluginval (Tracktion) for plugin/RT-safety validation"
echo "- REAPER or another lightweight DAW/plugin host"
echo "- Logic Pro if you want serious AU testing"
echo "- Apple Developer account later for signing/notarization"
