#!/usr/bin/env bash
set -euo pipefail

if [[ "${RUNNER_ENVIRONMENT:-}" != github-hosted || -n "${ACT:-}" ]]; then
    echo "free-disk: not a GitHub-hosted runner, leaving disk and images alone"
    exit 0
fi

df -h /
sudo rm -rf /usr/share/dotnet /usr/local/lib/android /opt/ghc /usr/local/.ghcup /opt/hostedtoolcache/CodeQL /usr/share/swift
sudo docker image prune --all --force >/dev/null
df -h /
