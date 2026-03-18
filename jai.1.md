% jai(1)
% David Mazieres
%

# NAME

jai - Jail an AI agent

# SYNOPSIS

`jai` [*option*]...  *cmd* [*arg*]... \
`jai` [*option*]... \
`jai` `-u`

# DESCRIPTION

`jai` is a super lightweight sandbox for AI agents requiring almost no
configuration.  By default it provides casual security, so is not a
substitute for using a proper container to confine agents.  However,
it is a great alternative to using no protection at all when you are
thinking of giving an agent full control of your account and all its
files.  Compared to the latter, `jai` can reduce the blast radius
should things go wrong.

By default, if you run "`jai` *cmd* [*arg*]...", it will execute *cmd*
with the specified arguments in a lightweight jail that has full
access to the current working directory and everything below,
copy-on-write access to an overlay mount of your home directory,
private `/tmp` and `/var/tmp` directories, and read-only access to
everything else.  This is known as _casual mode_, because *cmd* can
read most sensitive files on the system.  In other words, jai prevents
*cmd* from clobbering all your files, but doesn't provide much
confidentiality.

If you run `jai -mstrict` *cmd* [*arg*]...", then *cmd* will be run
with an empty home directory as an unprivileged user id, but with the
current working directory mapped to its place and fully exposed.
While the rest of the system outside the user's home directory is
available read-only, because *cmd* is running with a different user
ID, it will not be able to read sensitive files accessible to the
user.

If your home directory is on NFS, set the `JAI_CONFIG_DIR` environment
variable to a directory that you own on a local file system supporting
extended attributes.  Otherwise, overlay mounts may not work and you
may only be able to use bare mode (see below).

If you want to grant access to directories other than the current
working directory, you can specify addition directories with the `-d`
option, as in `jai -d /local/build untrusted_program`.  If you don't
want to grant access to the current working directory, use the `-D`
option.

If you use casual mode and forget to export some directory that you
wanted the mailed tool to update, you will find changed files in
`$HOME/.jai/default.changes`.  You can destroy the sandbox with `jai
-u`, move the changed files back into your home directory, and re-run
`jai` with the appropriate `-d` flag.

jai allows the use of multiple sandboxed home directories.  To use a
home directory other than the default, just give it a name with the
`-n` option and it will be created on demand.  When you specify a home
directory with `-n`, strict mode becomes the default (unless there is
no unprivileged `jai` user on your system, in which case it falls back
to bare mode).  It is possible to have multiple home overlays by
specifying `-mcasual` with `-n`.

# CONFIGURATION

If *cmd* does not contain any slashes, configuration is taken from
`$HOME/.jai/`*cmd*`.conf`, or, if no such file exists, from
`$HOME/.jai/default.conf`.  The format of the configuration file is a
series of lines of the form "*option* [*value*]".  *option* can be any
long command-line option without the leading `--`, for example:

    conf default.conf
    mode casual
    dir /local/build
    mask Mail

Within a configuration file, `conf` acts like an include directive,
logically replacing the `conf` line with the contents of another
configuration file.  (Relative paths are relative to `$HOME/.jai/`.)

jai executes programs with bash.  The `command` directive allows you
to reconfigure the environment or add command-line options to certain
commands.  For instance, you might create a file `python.conf` with
the following:

    conf default.conf
    mode strict
    dir venv
    name python
    command source $HOME/venv/bin/activate; "$0" "$@"

Then when running `jai python`, this configuration file will load a
virtual environment before running the command.

# EXAMPLES

    jai -d ~/.claude claude

    jai -d ~/.codex codex

    mkdir -p ~/.local/share/opencode
    jai -d ~/.config/opencode -d ~/.local/share/opencode opencode

# OPTIONS

`-C` *file*, `--conf `*file*
: Specifies the configuration file to read.  If *file* does not
  contain a `/`, the file is relative to `$HOME/.jai`.

  If no configuration file is specified, the default is based on the
  *cmd* argument.  If *cmd* contains no slashes and does not start
  with `.`, the system will use `$HOME/.jai/`*cmd*`.conf` if such a
  file exists.  Otherwise it uses `$HOME/.jai/default.conf`.

  Note that command-line arguments are parsed both before and after
  the file specified by the `-C` or `--conf` option.  Hence,
  command-line options always take precedence over configuration
  files.  When `conf` is specified in a configuration file, however,
  the behavior is different.  The specified file is read at the exact
  point of the `conf` directive, so overriding previous lines and
  getting overridden by subsequent lines.

`-d` *dir*, `--dir` *dir*
: Grant full access to directory *dir* and everything below in the
  jail.  You must own *dir*.  You can supply this option multiple
  times.  Note that on the command line, relative paths are relative
  to the current working directory, while in configuration files, they
  are relative to your home directory.

`-D`, `--nocwd`
: By default, `jai` grants access to the current working directory
  even if it is not specified with `-d`.  This option suppresses that
  behavior.  If you run with `-D` and no `-d` options, your entire
  home directory will be copy-on-write (in casual mode) or empty (in
  bare or strict mode) and nothing will be directly exported.

`-m casual`|`bare`|`strict`, `--mode casual`|`bare`|`strict`
: Set jai's execution mode.  In casual mode, the user's home directory
  is made available as an overlay mount.  Casual mode protects against
  destruction of files outside of granted directories, but does not
  protect confidentiality:  sandboxed code can read most files
  accessible to the user.  You can hide specific files with the
  `--mask` option or by deleting them under `/run/jai/$USER/*.home`,
  but because casual mode makes everything readable by default, it
  cannot protect all sensitive files.

    In strict mode, the user's home directory is replaced by an empty
  directory (`$HOME/.jai/`*name*`.home`), and sandboxed code runs with
  a different user id, `jai`.  Id-mapped mounts are used to map `jai`
  to the invoking user in granted directories.  Strict mode is the
  default when you name a sandbox (see `--name`), but not for the
  default sandbox.

    Bare mode uses an empty directory like strict mode, but runs with
  the invoking user's credentials.  It is inferior to strict mode, but
  can be used for NFS-mounted home directories, since NFS does not
  support id-mapped mounts.

`-n` *name*, `--name` *name*
: jai allows you to have multiple sandboxed home directories, which
  may be useful when sandboxing multiple tools that should not have
  access to each other's API keys.  This option specifies which home
  directory to use.  If no such sandbox exists yet, it will be created
  on demand.  When not specified, the default is just `default`.  Note
  that each name can be associated with both a casual home directory
  (accessible at `/run/jai/$USER/`*name*`.home`, with changes in
  `$HOME/.jai/`*name*`.changes`) and a strict/bare home directory (in
  `$HOME/.jai/`*name*`.home`).  There is no special relation between
  these home directories, but casual and strict sandboxes by the same
  name do share the same `/tmp` directory.

`--mask` *file*
: When creating an overlay home directory, create a "whiteout" file to
  hide *file* in the sandbox.  You can specify this option multiple
  times.  An easier way to hide files is just to delete them from
  `/run/jai/$USER/*.home`; hence, this option is mostly useful in
  configuration files to specify a set of files to delete by default.
  If you add `mask` directives to your configuration file, you will
  need to clear mounts with `jai -u` before the changes take effect.

`--unsetenv` *var*
: Filters *var* from the environment of the sandboxed program.  Can be
  the simple name of an environment variable, or can use the wildcard
  `*` as in `*_PID`.  (Since sandboxed processes don't see outside
  processes, you might as well filter any PIDs exposed in environment
  variables to avoid confusion.)

`--command` *bash-command*
: jai launches the sandboxed program you specify by running
  "`/bin/bash -c` *bash-command* *cmd* *arg*...".  By default,
  *bash-command* just runs the program as `"$0" "$@"`, but in
  configuration files for particular programs, you can use
  *bash-command* to set environment variables or add additional
  command-line options.

`-u`
: Unmounts all overlay directories from `/run/jai` and cleans up
  overlay-related files in `$HOME/.jai/*.work` that the user might not
  be able to clean up without root.  This option also destroys the
  private `/tmp` and `/var/tmp` directories (same directory at both
  mount points), so make sure you don't need anything in there.

  Many configuration choices apply only when a sandbox is first
  created, so you must run `jai -u` to change them.  In particular,
  you must use this option if you have added new files to be masked,
  as masking only takes effect at the time an overlay home is created.

`--version`
: Prints the version number and copyright.

# ENVIRONMENT

`SUDO_USER`, `USER`
: If jai is invoked with real UID 0 and either of these environment
  variables exists, it will be taken as the user whose home directory
  should be sandboxed.  This makes it convenient to run `jai` via
  `sudo` if you don't want to install it setuid root.  If both are
  set, `SUDO_USER` takes precedence.

`JAI_CONFIG_DIR`
: Location of jai configuration files and private home directories, by
  default `$HOME/.jai`.  If your home directory is on NFS, you may
  wish to put your private home directories elsewhere in order to use
  casual mode.

# FILES

In the following paths, the location `$HOME/.jai` can be changed by
setting the `JAI_CONFIG_DIR` environment variable.

`$HOME/.jai/default.conf`, `$HOME/.jai/`*cmd*`.conf`
: Configuration file if none is specified with `-C`.  If there is a
  file for *cmd*, then *cmd*`.conf` is used.  Otherwise `default.conf`
  is used.

`$HOME/.jai/default.changes`, `$HOME/.jai/`*name*`.changes`
: This "upper" directory is overlaid on your home directory and
  contains changes that have been made inside a jail.  If you make
  changes in this directory, you may need to tear down and recreate
  the sandboxed home directory with `jai -u`.  The non-default version
  is used when you specify `-n` *name* on the command line.

`$HOME/.jai/default.work`, `$HOME/.jai/`*name*`.work`
: This "work" directory is required by overlayfs, but does not contain
  anything user-accessible.  Every once in a while the overlay file
  system may create files in here that you cannot delete.  If you are
  trying to blow away an overlay directory to start from scratch and
  cannot delete this directory, try running `jay -u` which will clean
  things up.

`/run/jai/$USER/default.home`, `/run/jai/$USER/`*name*`.home`
: Sandboxed home directories for jails.  You can delete files with
  sensitive data in these sandboxed directories to hide theme from
  jailed processes, or see the `--mask` option.

`/run/jai/$USER/tmp/default`, `/run/jai/$USER/tmp/`*name*
: Private `/tmp` and `/var/tmp` directory made available in the jail.

# BUGS

Overlayfs needs an empty directory `$HOME/.jai/work`, into which it
places two root-owned directories `index` and `work`.  Usually these
directories are empty when the file system is unmounted.  However,
occasionally they contain files, in which case it requires root
privileges to delete the directories.  You can run `jai -u` to clean
these up if you are unable to delete them.

In general overlayfs can be flaky.  If the attributes on the
`default.changes` directory get out of sync, it may require making a
new `default.changes` directory to get around mounting errors.

There is no way to reverse an `unsetenv` or `mask` configuration
option.
