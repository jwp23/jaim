
#include "jai.h"

const std::string default_conf =
    R"(# Instead of copying this file to create a new configuration, you can
# it the file by referernce using a conf command.  Don't uncomment in
# this example line default.conf or you will create a loop.  Example:
#
# conf default.conf

# The default mode is strict for all named sandboxes and casual for
# the default sandbox.  A strict sandbox runs under the dedicated jai
# UID and starts with an empty home directory.  A casual sandbox runs
# with your own UID and makes your home directory copy-on-write via an
# overlay mount.  To change the default, you can uncomment one of the
# following:

# casual
# strict

# jai launches programs in a sandbox by running bash with the command
# name in "$0" and the arguments in "@".  bash will have a PID 1,
# which can confuse some programs.  Adding "; exit $?" after the
# command and arguments will cause bash to fork and stay around,
# giving "$0" PID 2.  For non-default configurations, you can also use
# a command directive to insert extra arguments or set environment
# variables.

command "$0" "$@"; exit $?

# Masked files are deleted when an overlayfs is first created, but
# have no effect on existing overlays.  To delete files from an
# existing overlay, delete them under /run/jai/$USER/default.home.
# Otherwise, to apply new mask directives you can run "jai -u" to
# unmount any existing overlays.

mask .jai
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
mask .config/chromium
mask .config/google-chrome
mask .config/BraveSoftware
mask .bash_history
mask .zsh_history

# The following environment variables will be removed from sandboxed
# environments.  You can use * as a wildcard to match any variables
# matching the pattern.

unsetenv AZURE_CLIENT_ID
unsetenv AZURE_TENANT_ID
unsetenv DATABASE_URL
unsetenv MONGO_URI
unsetenv MONGODB_URI
unsetenv REDIS_URL
unsetenv GOOGLE_APPLICATION_CREDENTIALS
unsetenv KUBECONFIG
unsetenv BB_AUTH_STRING
unsetenv SENTRY_DSN
unsetenv SLACK_WEBHOOK_URL
unsetenv *_ACCESS_KEY
unsetenv *_API_KEY
unsetenv *_APIKEY
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
)";
