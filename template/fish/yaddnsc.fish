# Fish completion for yaddnsc — Yet Another Dynamic DNS Client
# https://github.com/CyberKoo/yaddnsc
#
# Install to /usr/share/fish/vendor_completions.d/yaddnsc.fish (Debian/Ubuntu)
# or /usr/share/fish/completions/yaddnsc.fish and restart your shell.

# ── Global flags ────────────────────────────────────────────────────────────
complete -c yaddnsc -s v -l version -d "Print version information"

# ── Top-level subcommands ───────────────────────────────────────────────────
complete -c yaddnsc -n "not __fish_seen_subcommand_from run driver interface if net dns config info" \
    -f -a "run" -d "Run the DDNS client"
complete -c yaddnsc -n "not __fish_seen_subcommand_from run driver interface if net dns config info" \
    -f -a "driver" -d "Manage DDNS driver modules"
complete -c yaddnsc -n "not __fish_seen_subcommand_from run driver interface if net dns config info" \
    -f -a "interface" -d "Query network interfaces"
complete -c yaddnsc -n "not __fish_seen_subcommand_from run driver interface if net dns config info" \
    -f -a "dns" -d "DNS lookup and diagnostics"
complete -c yaddnsc -n "not __fish_seen_subcommand_from run driver interface if net dns config info" \
    -f -a "config" -d "Configuration management"
complete -c yaddnsc -n "not __fish_seen_subcommand_from run driver interface if net dns config info" \
    -f -a "info" -d "Show build configuration"

# ── run ─────────────────────────────────────────────────────────────────────
complete -c yaddnsc -n "__fish_seen_subcommand_from run" -s c -l config \
    -d "Path to configuration file" -r -F
complete -c yaddnsc -n "__fish_seen_subcommand_from run" -s d -l debug \
    -d "Enable verbose (debug) logging"

# ── driver ──────────────────────────────────────────────────────────────────
complete -c yaddnsc -n "__fish_seen_subcommand_from driver" -s c -l config \
    -d "Path to configuration file" -r -F
complete -c yaddnsc -n "__fish_seen_subcommand_from driver; and not __fish_seen_subcommand_from list info" \
    -f -a "list" -d "List all loaded drivers"
complete -c yaddnsc -n "__fish_seen_subcommand_from driver; and not __fish_seen_subcommand_from list info" \
    -f -a "info" -d "Show detailed information about a driver"
complete -c yaddnsc -n "__fish_seen_subcommand_from driver; and __fish_seen_subcommand_from list" \
    -s c -l config -d "Path to configuration file" -r -F
complete -c yaddnsc -n "__fish_seen_subcommand_from driver; and __fish_seen_subcommand_from info" \
    -s c -l config -d "Path to configuration file" -r -F

# ── interface (aliases: if, net) ────────────────────────────────────────────
complete -c yaddnsc -n "__fish_seen_subcommand_from interface if net; and not __fish_seen_subcommand_from list ip" \
    -f -a "list" -d "List all network interfaces"
complete -c yaddnsc -n "__fish_seen_subcommand_from interface if net; and not __fish_seen_subcommand_from list ip" \
    -f -a "ip" -d "Show IP addresses of a network interface"
# Dynamic interface name completion for 'interface ip'
complete -c yaddnsc -n "__fish_seen_subcommand_from interface if net; and __fish_seen_subcommand_from ip" \
    -f -a "(__fish_print_interfaces)" -d "Interface name"

# ── dns ─────────────────────────────────────────────────────────────────────
complete -c yaddnsc -n "__fish_seen_subcommand_from dns" -s c -l config \
    -d "Path to configuration file" -r -F
complete -c yaddnsc -n "__fish_seen_subcommand_from dns; and not __fish_seen_subcommand_from resolve resolver" \
    -f -a "resolve" -d "Resolve a hostname"
complete -c yaddnsc -n "__fish_seen_subcommand_from dns; and not __fish_seen_subcommand_from resolve resolver" \
    -f -a "resolver" -d "Show configured resolver details"
# dns resolve
complete -c yaddnsc -n "__fish_seen_subcommand_from dns; and __fish_seen_subcommand_from resolve r" \
    -s c -l config -d "Path to configuration file" -r -F
complete -c yaddnsc -n "__fish_seen_subcommand_from dns; and __fish_seen_subcommand_from resolve r" \
    -l type -d "DNS record type (A, AAAA, TXT)" -r -f -a "A AAAA TXT"
# dns resolver
complete -c yaddnsc -n "__fish_seen_subcommand_from dns; and __fish_seen_subcommand_from resolver" \
    -s c -l config -d "Path to configuration file" -r -F

# ── config ──────────────────────────────────────────────────────────────────
complete -c yaddnsc -n "__fish_seen_subcommand_from config" -s c -l config \
    -d "Path to configuration file" -r -F
complete -c yaddnsc -n "__fish_seen_subcommand_from config; and not __fish_seen_subcommand_from show test" \
    -f -a "show" -d "Print resolved configuration as JSON"
complete -c yaddnsc -n "__fish_seen_subcommand_from config; and not __fish_seen_subcommand_from show test" \
    -f -a "test" -d "Validate configuration file and exit"
# config show
complete -c yaddnsc -n "__fish_seen_subcommand_from config; and __fish_seen_subcommand_from show s" \
    -s c -l config -d "Path to configuration file" -r -F
# config test
complete -c yaddnsc -n "__fish_seen_subcommand_from config; and __fish_seen_subcommand_from test t" \
    -s c -l config -d "Path to configuration file" -r -F
complete -c yaddnsc -n "__fish_seen_subcommand_from config; and __fish_seen_subcommand_from test t" \
    -s q -l quiet -d "Suppress success message"
