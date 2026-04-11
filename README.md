![](./logo.svg "jaim logo")

# jaim - A lightweight sandbox for AI CLIs on macOS

`jaim` is a macOS port of [jai](https://jai.scs.stanford.edu), an
ultra-lightweight jail for AI command-line tools. It uses macOS
[Seatbelt](https://reverse.put.as/wp-content/uploads/2011/09/Apple-Sandbox-Guide-v1.0.pdf)
sandbox profiles to restrict what sandboxed processes can access, so you
can run AI assistants without giving them free rein over your system.

`jaim` *command* runs *command* with the following default policy:

* *command* has full read/write access to the current working directory.

* In **casual** mode (the default), *command* has read-only access to
  your home directory, with sensitive files masked (SSH keys, cloud
  credentials, browser data, shell history, etc.).

* In **bare** mode, *command* has read-only access to your home
  directory with a similar set of masked files.

* In **strict** mode, *command* has no access to your home directory
  at all, except the current working directory and any explicitly
  granted paths.

* `/tmp` and system temp directories are writable.

* The rest of the filesystem is read-only.

* Network access is unrestricted.

* Sensitive environment variables (tokens, passwords, API keys) are
  stripped automatically.

## Features

* **Three security modes** -- casual, bare, and strict -- to balance
  convenience and isolation.

* **Named sandboxes** -- multiple independent sandbox configurations
  that don't see each other's state.

* **Directory grants** -- grant read-only or full access to additional
  directories with `--dir`, `--rdir`, and related options.

* **File masking** -- deny access to specific paths within your home
  directory (`.ssh`, `.aws`, `.gnupg`, etc.) even in casual mode.

* **Environment filtering** -- automatically strips credentials and
  secrets from the sandboxed environment. Configurable with
  `--setenv` and `--unsetenv`.

* **Per-command configuration** -- configuration files in
  `~/.jaim/<command>.conf` let you set different policies for
  different tools.

* **Shell scripting** -- source custom bash functions into sandboxed
  shells via `~/.jaim/.jaimrc`.

* **No root required** -- unlike the Linux version, jaim uses the
  unprivileged macOS sandbox API.

## Requirements

* macOS on Apple Silicon (arm64)
* A C++ compiler with C++23 support (Xcode 16+ / Apple Clang 18+)

## Building and installing

### Quick build (recommended)

The included `GNUmakefile` builds jaim without autotools:

```sh
make
make install
```

This installs to `~/.local/bin` by default (no `sudo` required).
Make sure `~/.local/bin` is in your `PATH`:

```sh
export PATH="$HOME/.local/bin:$PATH"
```

To install to a different prefix:

```sh
make PREFIX=/usr/local install   # requires sudo
```

### Autotools build

If you prefer the autotools workflow:

```sh
./autogen.sh    # only from a git checkout
./configure
make
make install
```

### First run

After building, initialize your configuration:

```sh
./jaim --init
```

This creates `~/.jaim/` with default configuration files (`.defaults`,
`default.conf`, `default.jail`, `.jaimrc`). You can edit these to
customize the sandbox behavior.

## Usage

Run a command inside the sandbox:

```sh
jaim claude
jaim bash
jaim your-ai-tool --some-flag
```

Run with no arguments to get a sandboxed shell:

```sh
jaim
```

### Common options

```
-m, --mode casual|bare|strict   Set the sandbox mode
-d, --dir DIR                   Grant full access to DIR
-r, --rdir DIR                  Grant read-only access to DIR
-j, --jail NAME                 Use a named sandbox
-D, --nocwd                     Don't grant access to the current directory
-C, --conf FILE                 Use a specific configuration file
    --mask FILE                 Deny access to $HOME/FILE
    --unmask FILE               Undo a previous --mask
    --setenv VAR[=VALUE]        Set an environment variable
    --unsetenv VAR              Remove an environment variable (supports wildcards)
    --help                      Show full help
    --version                   Show version information
```

### Examples

Run Claude Code with access to an extra directory:

```sh
jaim -d ~/data claude
```

Run in strict mode with no home directory access:

```sh
jaim -m strict bash
```

Use a named sandbox to keep state separate:

```sh
jaim -j project-a claude
jaim -j project-b claude
```

## Configuration

Configuration lives in `~/.jaim/` (or `$JAIM_CONFIG_DIR`):

| File              | Purpose                                      |
|-------------------|----------------------------------------------|
| `.defaults`       | Base defaults included by other config files  |
| `default.conf`    | Default configuration (includes `.defaults`)  |
| `default.jail`    | Default sandbox settings (mode, etc.)         |
| `.jaimrc`         | Bash functions available in sandboxed shells  |
| `<name>.conf`     | Per-command configuration                     |
| `<name>.jail`     | Per-sandbox settings                          |

To view the built-in defaults:

```sh
jaim --print-defaults
```

## How it works

jaim uses the macOS Seatbelt sandbox (`sandbox_init(3)`) to enforce
file access restrictions. When you run `jaim command`:

1. jaim reads your configuration files to determine the sandbox mode,
   granted directories, masked files, and environment filters.

2. It generates a Seatbelt profile -- a set of allow/deny rules in
   Apple's Scheme-like sandbox profile language -- based on your
   configuration.

3. It forks a child process, applies the sandbox profile with
   `sandbox_init()`, sets up the environment, and execs the command.

4. The parent process waits for the child and propagates its exit
   status.

The sandbox is enforced by the kernel. Once applied, a process cannot
escape it or weaken its restrictions. Unlike containers or VMs, there
is no filesystem overhead -- jaim processes see the real filesystem,
just with access restrictions enforced by the kernel.

## Credits

jaim is a macOS port of [jai](https://jai.scs.stanford.edu) by David
Mazieres. The original jai targets modern Linux (kernel 6.13+) and
uses namespaces and overlayfs for isolation. jaim replaces those
Linux-specific mechanisms with macOS Seatbelt sandbox profiles.

## License

GNU General Public License v3 or later. See [COPYING](COPYING).
