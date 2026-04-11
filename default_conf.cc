/*
 * jaim - Sandboxing tool for AI CLI access
 * Copyright (C) 2026 David Mazieres
 * Copyright (C) 2026 Joseph Presley
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Modified 2026 by Joseph Presley: port from Linux to macOS arm64.
 * Modified 2026 by Joseph Presley: add default_claude_conf for the
 *   shipped Claude Code preset (ja-ofy).
 */

#include "jaim.h"

const std::string jaim_defaults =
    R"(# This file contains generic defaults built into jaim.  It is intended
# to be included by other configuration files with a line:
#
#     conf .defaults
#
# You can override settings in this file by editing it directly or by
# appending configuration directives to default.conf or other
# <name>.conf files after the `conf .defaults` line.  (Later lines
# override previous ones in configuration files.)
#
# If you delete this file, jaim will re-create it the next time it
# runs.  You can also see the default contents of this file by running
#
#     jaim --print-defaults


# By default, jaim stores configuration in $HOME/.jaim
# (or in $JAIM_CONFIG_DIR, if set).
# Environment variables named in ${...} will be substituted.

# storage /some/local/directory/${JAIM_USER}/.jaim

# The default mode is strict.  A strict sandbox denies all access to
# your home directory except the current working directory and any
# explicitly granted directories.  A casual sandbox allows read-only
# access to your home directory with sensitive files masked.  Bare
# mode is equivalent to casual on jaim (macOS has no overlay file
# system to distinguish them the way jai does on Linux).  Writes to
# the home directory should be granted explicitly with --dir.
# Uncomment any of the following to set the mode, or override it in
# individual .jail files:

# mode casual
# mode bare
# mode strict

# You can use "jail NAME" to specify different sandboxes.
# If you leave jail undefined, the name will be "default" and the
# mode will default to casual, but if you define this to anything
# including "default", then the default mode will be strict.

# jail default

# The script option loads bash source.  All files specified with
# script will be concatenated and placed in a single file that is
# designated by the $JAIM_SCRIPT environment variable.
#
# The command tells jaim to launch programs by running bash with the
# command name in "$0" and the arguments in "@".  Altering command
# allows you set environment variables dynamically or change
# command-line arguments.  The following settings enable shell aliases
# and source the file $HOME/.jaim/.jaimrc, into which you can store
# shell functions and command aliases.

script? .jaimrc
command source "${JAIM_SCRIPT:-/dev/null}"; "$0" "$@"

# Masked files are denied access by the sandbox profile.  In casual
# and bare modes, these files within your home directory will be
# inaccessible to sandboxed commands.  If you want to avoid masking
# any of these files in one particular configuration, you can use a
# directive such as `unmask .aws` to undo the effects from a
# previously included default file.

mask .jaim
# The entire ~/.ssh directory is denied by default so that private
# keys with any naming convention (id_ed25519_work, google_compute_engine,
# etc.) are blocked automatically.  jaim then allow-lists the specific
# files most SSH clients need: agent sockets under ~/.ssh/agent/*,
# known_hosts, known_hosts.old, and *.pub public keys.  If you need
# additional files accessible, grant them explicitly with --dir or
# use `unmask .ssh` to remove the blanket protection.
mask .ssh
mask .gnupg
mask .local/share/keyrings
mask .netrc
mask .git-credentials
mask .aws
mask .azure
mask .config/gcloud
mask .config/gh
mask .config/Keybase
# kubectl's default kubeconfig is ~/.kube/config, not ~/.config/kube
# — the Linux XDG-style path here was a miss.  Mask the whole ~/.kube
# directory so kubeconfig, cache/, and any tool-specific state under
# it are all denied; tools wanting a different path can use
# $KUBECONFIG (which is stripped by the unsetenv list below).
mask .kube
mask .docker
mask .password-store
# Credential-bearing files for common developer toolchains.  Each of
# these holds plaintext secrets (registry tokens, publish keys, DB
# passwords) that would otherwise be readable in casual mode.  Most
# are per-file masks rather than whole-directory so non-credential
# config alongside them (e.g. .cargo/config.toml) stays accessible.
mask .pgpass
mask .my.cnf
mask .npmrc
mask .yarnrc
mask .yarnrc.yml
mask .pypirc
mask .cargo/credentials.toml
mask .gem/credentials
mask .hex/hex.config
mask .config/helm/repositories.yaml
mask .config/rclone/rclone.conf
mask .mozilla
mask .config/BraveSoftware
mask .config/chromium
mask .config/google-chrome
mask .config/mozilla
# macOS browser data lives under ~/Library — not the XDG paths above —
# so the Linux-style masks are no-ops here.  These additional masks
# cover Safari's cookies/history, shared HTTP cookies, the login
# keychain, and the per-browser profile directories where Chrome,
# Firefox, Brave, Edge, Vivaldi, and Arc keep cookies, saved logins,
# and payment methods.  Paths with spaces are escaped so the config
# parser treats them as a single token.
mask Library/Safari
mask Library/Cookies
mask Library/Keychains
mask Library/Application\ Support/Google/Chrome
mask Library/Application\ Support/Firefox
mask Library/Application\ Support/BraveSoftware
mask Library/Application\ Support/com.microsoft.edgemac
mask Library/Application\ Support/Vivaldi
mask Library/Application\ Support/Arc
mask .bash_history
mask .zsh_history
# Modern shells store history under XDG or in their own dotfiles.
# Shell histories routinely contain one-liners with secrets pasted
# inline (curl with Authorization headers, env var exports, password
# prompts echoed by mistake), so treat them the same as bash/zsh.
mask .local/share/fish/fish_history
mask .local/share/nushell/history.txt
mask .local/share/nushell/history.sqlite3
mask .xonsh_history

# The following environment variables will be removed from sandboxed
# environments.  You can use * as a wildcard to match any variables
# matching the pattern.  If you want to undo any of these unsetenv
# commands in a particular config file, you can use setenv to reverse
# the effects of unsetenv.
#
# PRECEDENCE (read this before adding a setenv line to a config):
# A later `setenv VAR=VALUE` in ANY included config file silently
# overrides a previous `unsetenv VAR` or matching wildcard pattern,
# and the variable is passed into the sandbox.  Expansion of ${VAR}
# inside the value reads from the REAL environment at config-parse
# time, so a line like `setenv ANTHROPIC_API_KEY=${ANTHROPIC_API_KEY}`
# defeats the *_API_KEY strip below and leaks your real token into
# the sandbox.  jaim will warn (but not error) when a `setenv VAR=...`
# in a config file overrides an unsetenv pattern.

unsetenv *_ACCESS_KEY
unsetenv *_APIKEY
unsetenv *_API_KEY
unsetenv *_AUTH
unsetenv *_AUTH_TOKEN
unsetenv *_CONNECTION_STRING
unsetenv *_CREDENTIAL
unsetenv *_CREDENTIALS
unsetenv *_PASSWD
unsetenv *_PASSWORD
unsetenv *_PID
unsetenv *_PRIVATE_KEY
unsetenv *_PWD
unsetenv *_SECRET
unsetenv *_SECRET_KEY
unsetenv *_SOCK
unsetenv *_SOCKET
unsetenv *_SOCKET_PATH
unsetenv *_TOKEN
# Connection-string URL env vars routinely embed credentials inline
# (e.g. postgres://user:pass@host/db), but the "*_URL" suffix is too
# broad to strip generically — HOME_URL, PUBLIC_URL, and many other
# benign vars share it.  Enumerate the specific names in common use
# across Rails, Django, Celery, Sentry, and friends.  *_CONNECTION_STRING
# above catches the explicit-name convention; these cover everything
# else.  Upstream jai's defaults also miss these — report upstream.
unsetenv AMQP_URL
unsetenv AZURE_CLIENT_ID
unsetenv AZURE_TENANT_ID
unsetenv BB_AUTH_STRING
unsetenv CELERY_BROKER_URL
unsetenv CLICKHOUSE_URL
unsetenv DATABASE_URI
unsetenv DATABASE_URL
unsetenv DSN
unsetenv ELASTICSEARCH_URL
unsetenv GOOGLE_APPLICATION_CREDENTIALS
unsetenv INFLUXDB_URL
unsetenv KUBECONFIG
unsetenv MAIL
unsetenv MONGODB_URI
unsetenv MONGO_URI
unsetenv MONGO_URL
unsetenv RABBITMQ_URL
unsetenv REDIS_URI
unsetenv REDIS_URL
unsetenv SENTRY_DSN
unsetenv SLACK_WEBHOOK_URL

# The following environment variables get set in sandboxes.  You can
# substitute existing environment variables (before any
# unsetenv/setenv have been applied) by including them in ${...}.  You
# can reference ${JAIM_USER} here, which gets set before configuration,
# but not ${JAIM_JAIL} or ${JAIM_MODE}, which are set after.

setenv USER=${JAIM_USER}
setenv LOGNAME=${JAIM_USER}
)";

extern const std::string default_conf =
  R"(# The following line includes sensible defaults from the file
# .defaults.  You can override these defaults by appending
# configuration options to this file.  See the .defaults file or the
# jaim(1) man page for details.

conf .defaults

)";

extern const std::string default_jail =
  R"(# Set casual mode for the default sandbox.

mode casual

)";

extern const std::string default_claude_conf =
  R"(# Per-command configuration for the Anthropic Claude Code CLI.
#
# Casual mode makes $HOME read-only so that a confused or
# prompt-injected agent cannot delete or corrupt your files.  That
# protection, however, also blocks Claude Code's normal write paths
# for session state and user-level settings.  This config grants the
# minimum writes Claude Code needs while leaving the rest of $HOME
# (dotfiles, config, shell history, .ssh, .aws, browser data, etc.)
# read-only or masked as configured by default.conf.
#
# jaim picks this file up automatically when you run `jaim claude`,
# per the <name>.conf discovery rule documented in jaim(1).

conf default.conf

# User-level Claude Code tree.  Grants full read/write to the whole
# ~/.claude/ subtree, which includes:
#   ~/.claude/projects/.../*.jsonl  - session transcripts (append per turn)
#   ~/.claude/CLAUDE.md             - user-level memory / preferences
#   ~/.claude/settings.json         - user-level settings incl. MCP servers
#   ~/.claude/commands/             - user-level slash commands
#   ~/.claude/compact/              - compact caches
# The common case for users who manage MCP servers, edit their
# CLAUDE.md, or customize settings via Claude Code itself.
dir .claude

# Global Claude Code config (conversation index, permissions, MCP
# runtime state) lives in the home root, not under ~/.claude/, so it
# needs its own grant.  Claude Code uses atomic writes for this file,
# so the `file` directive also permits .claude.json.<suffix> sibling
# temp files used by the rename step.
file .claude.json
)";

extern const std::string default_jaimrc =
  R"(# -*- shell-script-mode -*-

# You can use this file to define bash functions invocable from jaim.
#
# To use the file, you will need the following in your .defaults file:
#
# command source "${JAIM_SCRIPT:-/dev/null}"; "$0" "$@"

# Here is an example of how to define a bash function:
#
#      lscolor() {
#        ls --color "$@"
#      }
#
# or
#
#      lscolor() { ls --color "$@"; }
#
# The special variable "$@" gets expanded to all the parameters.  Note
# that one-liner functions require a semicolon before the closing
# brace.

# Here are some example functions for people who like to live
# dangerously.  These modes are not recommended, but if you are going
# to use them anyway, better to define the dangerous functions in your
# .jaimrc so you can only invoke them in jaim environments.

# claudeyolo() { claude --dangerously-skip-permissions "$@"; }

# codexyolo() { codex --dangerously-bypass-approvals-and-sandbox "$@"; }
)";
