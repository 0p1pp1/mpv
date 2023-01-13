# DEMULTI2_MODE: select decoding-lib to be used from libdemulti2.
#
# Values: one of 'yakisoba', 'sobacas', 'pcsc'
#  'yakisoba': libyakisoba.so (software only decoding)
#  'sobacas':  libsobacas.so (wrapper lib for libyakisoba, with PC/SC API)
#  'pcsc':     libpcsclite.so (use PC/SC library and physical CARD reader)
#
# Usually no need to set this env variable,
# as libdemulti2 automatically probe those libs and use the first one found.
# (in the order of libyakisoba, libsobacas, libpcsclite).
#
# This env var overrides/forces the selection.

export DEMULTI2_MODE=${DEMULTI2_MODE:-yakisoba}

# BCAS_KEYS_FILE: path to the conf file (listing of Kw's) for libyakisoba.
#  if not set, defaults to $HOME/.bcas_keys, /etc/bcas_keys

DEFKEYCFG="${XDG_CONFIG_HOME:-${HOME}/.config}"/mpv/bcas_keys
export BCAS_KEYS_FILE="${BCAS_KEYS_FILE:-${DEFKEYCFG}}"
