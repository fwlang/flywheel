# Installing Flywheel

The installer builds Flywheel from source and installs the `flywheel` compiler with its runtime archive. SQLite and TLS (HTTPS) are compiled in, so programs need no OpenSSL or SQLite on the machine.

To install the package manager and the editor extension as well, see [Pulley](https://github.com/fwlang/pulley/blob/main/INSTALLATION.md) and [the VS Code extension](https://github.com/fwlang/vscode-fw/blob/main/INSTALLATION.md), or pass `--all` / `-All` to the installer.

## Linux

```sh
curl -fsSL https://flywheel.polymorphia.dev/install.sh | bash
```

The script installs missing build tools with your package manager (after asking), builds Flywheel, installs it into `~/.local/flywheel`, and adds it to `PATH`. It supports Arch, Debian/Ubuntu, and Fedora (and their derivatives). On other distributions it tells you what to install.

Pass options after `bash -s --`:

```sh
# Install Flywheel, Pulley, and the VS Code extension
curl -fsSL https://flywheel.polymorphia.dev/install.sh | bash -s -- --all

# Build a release tag instead of the latest main
curl -fsSL https://flywheel.polymorphia.dev/install.sh | bash -s -- --ref v2.3

# Show what would happen without changing anything
curl -fsSL https://flywheel.polymorphia.dev/install.sh | bash -s -- --dry-run

# List every option
curl -fsSL https://flywheel.polymorphia.dev/install.sh | bash -s -- --help
```

Optional: for desktop windows (`flywheel desktop` and the `desktop` module), install GTK and WebKitGTK before running the installer:

| Distribution  |               Packages               |
|---------------|--------------------------------------|
|     Arch      |         `gtk3 webkit2gtk-4.1`        |
| Debian/Ubuntu | `libgtk-3-dev libwebkit2gtk-4.1-dev` |
|     Fedora    |   `gtk3-devel webkit2gtk4.1-devel`   |

## Windows

In PowerShell (no administrator rights needed):

```powershell
irm https://flywheel.polymorphia.dev/install.ps1 | iex
```

If no Visual Studio C++ tools are installed, the script installs the free Build Tools for Visual Studio 2022 with winget (after asking). It gets LLVM, CMake, and Ninja from conda-forge into `%LOCALAPPDATA%\flywheel-install`, and installs Flywheel into `%LOCALAPPDATA%\Programs\Flywheel`.

To pass options, run it as a scriptblock:

```powershell
# Install Flywheel, Pulley, and the VS Code extension
& ([scriptblock]::Create((irm https://flywheel.polymorphia.dev/install.ps1))) -All

# List every option
& ([scriptblock]::Create((irm https://flywheel.polymorphia.dev/install.ps1))) -Help
```

The script also adds `flywheel-shell`, which opens a prompt with the Visual Studio environment that `flywheel run` and `flywheel build` need. Run Flywheel programs from there.

## Check it works

Open a new terminal, then:

```sh
flywheel --version
```

## Updating

Run the install command again.

## Uninstalling

```sh
curl -fsSL https://flywheel.polymorphia.dev/install.sh | bash -s -- --uninstall
```

On Windows, run the script with `-Help` to see its uninstall option.

## Troubleshooting

| Problem | Fix |
|---|---|
| `flywheel: command not found` | Open a new terminal so the updated `PATH` takes effect |
| `flywheel build` cannot find a linker (Windows) | Run it from `flywheel-shell` |
| HTTPS requests fail with "no trusted root certificates" | Install your distribution's `ca-certificates` package, or set `SSL_CERT_FILE` to a CA bundle |
| `DesktopError` when opening a window | Install the GTK/WebKitGTK packages (Linux) or the WebView2 runtime (Windows), then run the installer again |
