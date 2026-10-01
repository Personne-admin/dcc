#!/usr/bin/env bash
set -euo pipefail

df -h /
sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc /usr/local/.ghcup /opt/hostedtoolcache/CodeQL /usr/share/swift
sudo docker image prune --all --force >/dev/null
df -h /
