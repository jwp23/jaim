
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
# explicitly granted directories.  A casual sandbox allows full access
# to your home directory but masks sensitive files.  Bare mode allows
# only read access to the home directory with the same masks.
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
mask .config/kube
mask .docker
mask .password-store
mask .mozilla
mask .config/BraveSoftware
mask .config/chromium
mask .config/google-chrome
mask .config/mozilla
mask .bash_history
mask .zsh_history

# The following environment variables will be removed from sandboxed
# environments.  You can use * as a wildcard to match any variables
# matching the pattern.  If you want to undo any of these unsetenv
# commands in a particular config file, you can use setenv to reverse
# the effects of unsetenv.

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
unsetenv AZURE_CLIENT_ID
unsetenv AZURE_TENANT_ID
unsetenv BB_AUTH_STRING
unsetenv DATABASE_URL
unsetenv GOOGLE_APPLICATION_CREDENTIALS
unsetenv KUBECONFIG
unsetenv MAIL
unsetenv MONGODB_URI
unsetenv MONGO_URI
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
