![](./logo.png "jaim logo")

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
* **Xcode Command Line Tools** -- install with `xcode-select --install`.
  This provides the `clang++` compiler. jaim needs C++23 support
  (Xcode 16+ / Apple Clang 18+); if the build fails with errors about
  unknown C++ features, update Xcode from the App Store.
* *Optional:* `autoconf` and `automake` -- only needed for the autotools
  build (`brew install autoconf automake`).
* *Optional:* `pandoc` -- only needed to regenerate the `jaim.1` man
  page from `jaim.1.md` (`brew install pandoc`).

## Building and installing

### Quick build (recommended)

Nothing from Homebrew is required for this path -- Xcode Command Line
Tools is sufficient. The included `GNUmakefile` builds jaim without
autotools:

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

If you prefer the autotools workflow, first install the prerequisites:

```sh
brew install autoconf automake
```

Then build:

```sh
./autogen.sh    # only from a git checkout
./configure
make
make install
```

### First run

After building, initialize your configuration:

```sh
jaim --init
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
-F, --file FILE                 Grant write access to a specific FILE
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

Run Claude Code.  On first run, jaim creates `~/.jaim/claude.conf`
with the grants Claude Code needs (write access to `~/.claude/` for
session state and user-level settings, plus `~/.claude.json` for its
global config), so casual-mode home protection stays intact while
Claude Code can still persist its state:

```sh
jaim claude
```

Give Claude Code access to an extra data directory on top of the
defaults:

```sh
jaim -d ~/data claude
```

Casual mode makes `$HOME` read-only.  Tools other than the ones
jaim ships presets for (see **Using jaim with coding agents** below)
need their state paths granted explicitly.  Use `-d` for
directories and `-F` for individual files:

```sh
jaim -d ~/.config/mytool -F ~/.mytoolrc mytool
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

### Using jaim with coding agents

jaim exists to jail coding agents, and casual mode is read-only on
`$HOME` so a confused or prompt-injected agent cannot delete your
files.  Agents still need to write *something* in `$HOME` — session
history, user-level config, MCP server settings — so each supported
agent ships a per-command config file that grants those specific
paths and nothing else.

**Claude Code** (`claude`): `~/.jaim/claude.conf` ships with jaim.
Grants `~/.claude/` (projects, `CLAUDE.md`, `settings.json`, MCP
servers) and `~/.claude.json` (global config).  Running `jaim claude`
picks it up automatically.

**Other agents** (`aider`, `codex`, `cursor-cli`, ...): create a
`~/.jaim/<name>.conf` modeled on `claude.conf`.  Use `sudo fs_usage
-w -f filesys | grep <name>` from another terminal while the tool
runs to discover the exact paths it writes, then add `dir` /
`file` directives for each one.

## Configuration

Configuration lives in `~/.jaim/` (or `$JAIM_CONFIG_DIR`):

| File              | Purpose                                      |
|-------------------|----------------------------------------------|
| `.defaults`       | Base defaults included by other config files  |
| `default.conf`    | Default configuration (includes `.defaults`)  |
| `default.jail`    | Default sandbox settings (mode, etc.)         |
| `.jaimrc`         | Bash functions available in sandboxed shells  |
| `claude.conf`     | Per-command config for Claude Code (shipped)  |
| `<name>.conf`     | Per-command configuration                     |
| `<name>.jail`     | Per-sandbox settings                          |

Configuration files accept the same directives as the command line
(without the leading dashes).  The most useful for tailoring a
sandbox are:

| Directive        | Purpose                                           |
|------------------|---------------------------------------------------|
| `mode casual...` | Set the sandbox mode                              |
| `mask FILE`      | Deny access to `$HOME/FILE`                       |
| `unmask FILE`    | Undo a previous `mask`                            |
| `dir DIR`        | Grant full access to a directory                  |
| `rdir DIR`       | Grant read-only access to a directory             |
| `file FILE`      | Grant write access to a specific file             |
| `setenv VAR=..`  | Set an environment variable                       |
| `unsetenv VAR`   | Strip an environment variable (supports `*`)      |

The `file` directive is atomic-write aware: it grants access to
`FILE` itself and to any `FILE.<suffix>` temp sibling used by
libraries like node's `write-file-atomic` or Python's `atomicwrites`.
This is tighter than granting the whole parent directory and lets
you unblock a single dotfile in the home root without exposing the
rest of `$HOME`.

To view the built-in defaults:

```sh
jaim --print-defaults
```

### setenv/unsetenv precedence

The built-in defaults include an `unsetenv` list that strips
credentials (`*_API_KEY`, `*_TOKEN`, `DATABASE_URL`, etc.) from the
sandbox environment. A `setenv VAR=VALUE` line in any config file
(`.defaults`, `default.conf`, `<name>.conf`, etc.) **unconditionally
overrides** any prior `unsetenv` for `VAR`, including matching
wildcard patterns. In particular, a line like

```
setenv ANTHROPIC_API_KEY=${ANTHROPIC_API_KEY}
```

expands `${ANTHROPIC_API_KEY}` from the *real* environment at
config-parse time and passes the token into the sandbox, silently
defeating the default `*_API_KEY` strip. jaim will emit a warning
to stderr whenever a `setenv VAR=...` in a config file overrides an
`unsetenv` pattern (command-line `--setenv` never warns, since it
reflects explicit user intent).

## Threat model

jaim prevents **accidents**, not **attacks**. It is designed to stop
an AI tool from damaging your system through bugs or careless commands
-- not to contain a deliberately malicious or prompt-injected agent.

**What jaim protects against:**

* An agent running `rm -rf` in the wrong place.
* Unintended writes outside the current working directory (for example
  to `~/.bashrc` or `~/.ssh/config`).
* Reads of masked sensitive files (SSH keys, cloud credentials,
  browser data, shell history).

**What jaim does NOT protect against:**

* **Data exfiltration over the network.** Network access is
  unrestricted in every mode, including `strict`. Anything an agent
  can read -- the current working directory, git history, unmasked
  parts of your home directory -- it can POST to any host on the
  internet. Seatbelt does not block outbound connections, and jaim
  does not currently emit `(deny network*)` rules.
* **A compromised or prompt-injected agent.** If an agent decides to
  ship the contents of your working directory to an attacker, no jaim
  mode will stop it.
* **Secrets in the current working directory.** jaim grants full
  read/write access to the CWD. Anything sitting there -- `.env`
  files, API tokens, credentials in sibling git repos -- is readable
  by the agent and therefore exfiltratable.

### Recommendations for untrusted agents

* Use `strict` mode to deny all home directory access.
* Run jaim from a directory that contains only what the agent needs.
  Do not point jaim at a directory that holds secrets, credentials,
  or unrelated git repos.
* Add an outbound firewall (Little Snitch, LuLu, or pf rules) if you
  need containment against network exfiltration. jaim does not
  currently expose a flag to deny network.
* Treat jaim as a seatbelt, not a vault. For adversarial AI workloads,
  a VM or a separate user account gives stronger isolation.

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
