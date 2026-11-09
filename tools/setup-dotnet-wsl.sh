#!/usr/bin/env bash
# .NET 10 SDK + PowerShell 7 inside WSL, for SharpProspero's NativeAOT compile
set -euo pipefail
apt-get install -y -qq clang lld zlib1g-dev >/dev/null 2>&1 || true

if ! command -v dotnet >/dev/null; then
    curl -sSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh
    bash /tmp/dotnet-install.sh --channel 10.0 --install-dir /opt/dotnet
    ln -sf /opt/dotnet/dotnet /usr/local/bin/dotnet
fi

if ! command -v pwsh >/dev/null; then
    v=7.4.6
    curl -sSL "https://github.com/PowerShell/PowerShell/releases/download/v$v/powershell-$v-linux-x64.tar.gz" -o /tmp/pwsh.tgz
    mkdir -p /opt/pwsh && tar -xzf /tmp/pwsh.tgz -C /opt/pwsh
    chmod +x /opt/pwsh/pwsh
    ln -sf /opt/pwsh/pwsh /usr/local/bin/pwsh
fi

dotnet --version
pwsh -v
