# Installing Flywheel

Flywheel is built from source with CMake. The result is a `flywheel` compiler plus a runtime archive that programs link against. SQLite and TLS (HTTPS) are compiled into that archive, so programs need no OpenSSL or SQLite on the machine.

To install the package manager and the editor extension as well, see [Pulley](https://github.com/fwlang/pulley/blob/main/INSTALLATION.md) and [the VS Code extension](https://github.com/fwlang/vscode-fw/blob/main/INSTALLATION.md).

## Quick install

`install.sh` does the Linux steps below for you. It installs missing build tools with your package manager (after asking), builds Flywheel, installs it into `~/.local/flywheel`, and adds it to `PATH`. It can also install Pulley and the VS Code extension.

```sh
curl -fsSL https://flywheel.polymorphia.dev/install.sh | bash
```

Add options after `bash -s --`, for example `| bash -s -- --all` to install all three, or `--ref v2.3` to build a tag. `--help` lists every option, `--dry-run` shows what would happen, and `--uninstall` removes everything again. The script supports Arch, Debian/Ubuntu, and Fedora (and their derivatives). On other distributions it tells you what to install.

On Windows, `install.ps1` does the same from PowerShell (no administrator rights needed):

```powershell
irm https://flywheel.polymorphia.dev/install.ps1 | iex
```

If no Visual Studio C++ tools are installed, it installs the free Build Tools for Visual Studio 2022 with winget (after asking). It gets LLVM, CMake, and Ninja from conda-forge into `%LOCALAPPDATA%\flywheel-install`, and installs Flywheel into `%LOCALAPPDATA%\Programs\Flywheel`. For options, run it as a scriptblock, for example `& ([scriptblock]::Create((irm https://flywheel.polymorphia.dev/install.ps1))) -All`. `-Help` lists them. The script also adds `flywheel-shell`, which opens a prompt with the Visual Studio environment that `flywheel run` and `flywheel build` need.

|          |               Linux               |                      Windows                      |
|----------|-----------------------------------|---------------------------------------------------|
|  Status  |        Tested (Arch Linux)        |                Tested (Windows 11)                |
| Compiler |      GCC or Clang with C++20      | Visual Studio 2022 (Desktop development with C++) |
|   LLVM   | 21, 22, or 23 development package |            conda-forge `llvmdev` 21–23            |
|  Other   | CMake 3.24+, Ninja (recommended)  |     CMake 3.24+, Ninja (both come with conda)     |

The first configure downloads SQLite and Mbed TLS (about 10 MB), so it needs network access. For offline builds, see [Offline builds](#offline-builds).

## Linux

### 1. Install the build tools

**Arch Linux**

```sh
sudo pacman -S --needed base-devel cmake ninja llvm
```

**Debian / Ubuntu**

Most releases ship an older LLVM, so add the LLVM project's repository and install LLVM 21:

```sh
sudo apt install build-essential cmake ninja-build wget lsb-release gnupg
wget https://apt.llvm.org/llvm.sh
sudo bash llvm.sh 21
sudo apt install llvm-21-dev zlib1g-dev libzstd-dev
```

**Fedora**

```sh
sudo dnf install gcc-c++ cmake ninja-build llvm-devel zlib-devel libzstd-devel
```

Check the version with `llvm-config --version`. It must be 21, 22, or 23. Debian/Ubuntu install the tool as `llvm-config-21`.

**Optional:**

- `mold` or `lld` makes `flywheel build` link faster. It is used automatically when found.
- To use desktop windows (`flywheel desktop` command and `desktop` module), install GTK and WebKitGTK:

  | Distribution  |               Packages               |
  |---------------|--------------------------------------|
  |     Arch      |         `gtk3 webkit2gtk-4.1`        |
  | Debian/Ubuntu | `libgtk-3-dev libwebkit2gtk-4.1-dev` |
  |     Fedora    |   `gtk3-devel webkit2gtk4.1-devel`   |

  Without them, everything else still builds. Only `window.run()` raises `DesktopError`.

### 2. Get the source

```sh
git clone https://github.com/fwlang/flywheel.git
cd flywheel
```

This gets the latest `main`. To build a release instead, check out its tag, for example `git checkout v2.3`.

### 3. Build

From the `flywheel` directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build --parallel
```

On Debian/Ubuntu, use `-DLLVM_DIR=/usr/lib/llvm-21/lib/cmake/llvm` instead.

### 4. Install

```sh
cmake --install build --prefix "$HOME/.local/flywheel"
```

Then add the compiler to your `PATH`, for example in `~/.bashrc` or `~/.zshrc`:

```sh
export PATH="$HOME/.local/flywheel/bin:$PATH"
```

Any prefix works. To install for every user, use `--prefix /usr/local` with `sudo`. The installed layout is:

```text
<prefix>/bin/flywheel
<prefix>/lib/libflywheel_runtime.a
<prefix>/lib/libflywheel_desktop.so   (when desktop support was built)
<prefix>/modules/                     (packages installed globally by Pulley)
```

Keep the LLVM package installed. The compiler links against LLVM's shared library, and `flywheel build` uses the system C++ compiler to link programs.

### 5. Check it works

```sh
flywheel --version
flywheel run examples/hello.fly
```

## Windows

These steps match the Windows CI job. The Windows code paths haven't been tested by hand yet, so please report anything that fails.

### 1. Install the build tools

1. Install **Visual Studio 2022** (Community is fine) or the **Build Tools for Visual Studio 2022**, with the **Desktop development with C++** workload.
2. Install **Miniforge** (<https://github.com/conda-forge/miniforge>), or `micromamba`. You'll use it to get an LLVM package built for MSVC.
3. Open **x64 Native Tools Command Prompt for VS 2022** from the Start menu. All remaining steps run in this prompt.
4. Create an environment with LLVM, its compression libraries, CMake, and Ninja:

   ```bat
   conda create -n flywheel -c conda-forge llvmdev=21.1.8 zlib zstd cmake ninja
   conda activate flywheel
   ```

Optional: desktop windows use WebView2. The build fetches what it needs. The WebView2 runtime is already part of Windows 11 and current Windows 10. Elsewhere, install the Evergreen runtime from Microsoft.

### 2. Get the source

In the same prompt:

```bat
git clone https://github.com/fwlang/flywheel.git
cd flywheel
```

Git comes with Visual Studio 2022. If `git` isn't found, install it with `winget install Git.Git` and open a new prompt. To build a release instead of the latest `main`, check out its tag, for example `git checkout v2.3`.

### 3. Build

From the `flywheel` directory, in the same prompt:

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH="%CONDA_PREFIX%;%CONDA_PREFIX%\Library"
cmake --build build --parallel
```

### 4. Install

```bat
cmake --install build --prefix "%LOCALAPPDATA%\Programs\Flywheel"
```

Add `%LOCALAPPDATA%\Programs\Flywheel\bin` to your user `PATH`. Use **Settings → System → About → Advanced system settings → Environment Variables**, or:

```powershell
[Environment]::SetEnvironmentVariable("Path",
  [Environment]::GetEnvironmentVariable("Path", "User") + ";$env:LOCALAPPDATA\Programs\Flywheel\bin", "User")
```

Open a new terminal afterwards.

On Windows, both `flywheel run` and `flywheel build` link programs with the MSVC toolchain. Run them from a **Developer Command Prompt** or **Developer PowerShell for VS 2022**. If your LLVM package uses DLLs, keep its `bin` directory on `PATH` too.

### 5. Check it works

```bat
flywheel --version
flywheel run examples\hello.fly
```

## Updating

Run `git pull` in the `flywheel` directory (or check out the new tag), then repeat the build and install steps. CMake reuses the `build` directory and the downloaded SQLite and Mbed TLS.

## Offline builds

Download these two archives on a machine with network access, and extract them:

- SQLite 3.54.1: <https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip>
- Mbed TLS 4.1.1: <https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.1/mbedtls-4.1.1.tar.bz2>

Then point the configure step at the extracted copies:

```sh
cmake -S . -B build ... \
  -DFETCHCONTENT_SOURCE_DIR_SQLITE=/path/to/sqlite-amalgamation-3540100 \
  -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/path/to/mbedtls-4.1.1
```

On Windows, desktop support also downloads webview. Add `-DFETCHCONTENT_SOURCE_DIR_WEBVIEW=...` or `-DFLYWHEEL_DESKTOP=OFF`.

## Build options

| Option | Values | Effect |
|---|---|---|
| `CMAKE_BUILD_TYPE` | `Release`, `Debug`, ... | Use `Release` for an installed compiler |
| `FLYWHEEL_DESKTOP` | `AUTO` (default), `ON`, `OFF` | Build the desktop window backend when its dependencies are found, always, or never |
| `FLYWHEEL_LINKER` | `auto` (default), `mold`, `lld`, `default` | Linker used by `flywheel build` on Linux |

## Running the tests (optional)

The test suite needs Python 3. The HTTPS tests also need the `openssl` command, which only creates test certificates.

```sh
ctest --test-dir build --output-on-failure
```

## Uninstalling

Delete the install prefix (for example `~/.local/flywheel` or `%LOCALAPPDATA%\Programs\Flywheel`) and remove it from `PATH`. Packages that Pulley installed globally live in its `modules` directory and go with it.

## Troubleshooting

| Problem | Fix |
|---|---|
| `Could not find a package configuration file provided by "LLVM"` | Pass `-DLLVM_DIR=...` (Linux) or `-DCMAKE_PREFIX_PATH=...` (Windows), as shown above |
| `The link interface of target "LLVMSupport" contains: ZLIB::ZLIB but the target was not found` (or `zstd::...`) | Install the zlib and zstd development files: `conda install -c conda-forge zlib zstd` (Windows), `zlib1g-dev libzstd-dev` (Debian/Ubuntu), or `zlib-devel libzstd-devel` (Fedora). Then delete the `build` directory and configure again |
| `Flywheel requires LLVM 21 through 23` | Install a supported LLVM and point `LLVM_DIR` at it |
| Configure fails while downloading SQLite or Mbed TLS | Check network access or a proxy, or use [offline builds](#offline-builds) |
| `cannot locate Flywheel runtime archive` | `bin/flywheel` and `lib/` must stay together in the same prefix; reinstall instead of copying only the binary |
| `flywheel build` cannot find a linker (Windows) | Run it from a Developer Command Prompt for VS 2022 |
| HTTPS requests fail with "no trusted root certificates" | Install your distribution's `ca-certificates` package, or set `SSL_CERT_FILE` to a CA bundle |
| `DesktopError` when opening a window | Install the GTK/WebKitGTK packages (Linux) or the WebView2 runtime (Windows), then rebuild |
