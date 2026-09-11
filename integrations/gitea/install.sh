#!/usr/bin/env bash
set -euo pipefail

GITEA_DIR="${GITEA_DIR:-${1:-/opt/homelab/data/gitea/gitea}}"
DOCKER_COMPOSE_DIR="${DOCKER_COMPOSE_DIR:-${2:-/opt/homelab}}"
GITEA_UID="${GITEA_UID:-1000}"
GITEA_GID="${GITEA_GID:-1000}"

RESTART_MODE="prompt"
UNINSTALL=0

for arg in "$@"; do
	case "$arg" in
	--restart) RESTART_MODE="always" ;;
	--no-restart) RESTART_MODE="never" ;;
	--uninstall) UNINSTALL=1 ;;
	-h | --help)
		sed -n '2,20p' "$0"
		exit 0
		;;
	esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ASSET_SRC_DIR="$SCRIPT_DIR"
CSS_SRC="$SCRIPT_DIR/prism-gitea.css"
CORE_SRC="$SCRIPT_DIR/vendor/prism-core.min.js"

ASSET_DIR="$GITEA_DIR/public/assets/custom/dc"
TPL_DIR="$GITEA_DIR/templates/custom"
FOOTER="$TPL_DIR/footer.tmpl"

BEGIN_MARKER="<!-- BEGIN dcc-dc-syntax -->"
END_MARKER="<!-- END dcc-dc-syntax -->"

log() { printf '==> %s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
die() {
	printf 'ERROR: %s\n' "$*" >&2
	exit 1
}

write_block_file() {
	cat >"$1" <<'SNIPPET'
<link rel="stylesheet" href="{{AppSubUrl}}/assets/custom/dc/prism-gitea.css">
<script defer src="{{AppSubUrl}}/assets/custom/dc/prism-core.min.js"></script>
<script defer src="{{AppSubUrl}}/assets/custom/dc/grammar.js"></script>
<script defer src="{{AppSubUrl}}/assets/custom/dc/gitea-hook.js"></script>
SNIPPET
}

merge_footer() {
	local block_tmp
	block_tmp="$(mktemp)"
	write_block_file "$block_tmp"

	if [ ! -f "$FOOTER" ]; then
		log "creating $FOOTER"
		{
			printf '%s\n' "$BEGIN_MARKER"
			cat "$block_tmp"
			printf '%s\n' "$END_MARKER"
		} >"$FOOTER"
		rm -f "$block_tmp"
		return
	fi

	if grep -qF "$BEGIN_MARKER" "$FOOTER"; then
		log "refreshing managed block in existing $FOOTER"
		local out_tmp
		out_tmp="$(mktemp)"
		awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" -v snippet="$block_tmp" '
      $0 == begin {
        print
        while ((getline line < snippet) > 0) print line
        inblock = 1
        next
      }
      $0 == end {
        inblock = 0
        print
        next
      }
      !inblock { print }
    ' "$FOOTER" >"$out_tmp"
		mv "$out_tmp" "$FOOTER"
	else
		log "appending managed block to existing $FOOTER"
		{
			printf '\n%s\n' "$BEGIN_MARKER"
			cat "$block_tmp"
			printf '%s\n' "$END_MARKER"
		} >>"$FOOTER"
	fi
	rm -f "$block_tmp"
}

remove_managed() {
	if [ -f "$FOOTER" ] && grep -qF "$BEGIN_MARKER" "$FOOTER"; then
		log "removing managed block from $FOOTER"
		local out_tmp
		out_tmp="$(mktemp)"
		awk -v begin="$BEGIN_MARKER" -v end="$END_MARKER" '
      $0 == begin { inblock = 1; next }
      $0 == end { inblock = 0; next }
      !inblock { print }
    ' "$FOOTER" >"$out_tmp"
		mv "$out_tmp" "$FOOTER"
	fi
	if [ -d "$ASSET_DIR" ]; then
		log "removing $ASSET_DIR"
		rm -rf "$ASSET_DIR"
	fi
}

fix_perms() {
	chmod 755 "$GITEA_DIR" "$GITEA_DIR/public" "$GITEA_DIR/templates" "$ASSET_DIR" "$TPL_DIR" 2>/dev/null || true
	chmod 644 "$ASSET_DIR"/* "$FOOTER" 2>/dev/null || true
	if [ "$(id -u)" = "0" ]; then
		log "setting ownership to $GITEA_UID:$GITEA_GID"
		chown -R "$GITEA_UID:$GITEA_GID" "$ASSET_DIR" "$TPL_DIR"
	else
		warn "not running as root; skipping chown."
	fi
}

find_compose() {
	local d="$1" f
	for f in docker-compose.yml docker-compose.yaml compose.yml compose.yaml; do
		if [ -f "$d/$f" ]; then
			printf '%s' "$d/$f"
			return 0
		fi
	done
	return 1
}

maybe_restart() {
	local compose_file=""
	compose_file="$(find_compose "$DOCKER_COMPOSE_DIR" || true)"
	if [ -z "$compose_file" ]; then
		warn "no compose file found in $DOCKER_COMPOSE_DIR; restart gitea manually."
		return 0
	fi
	local do_restart=0
	case "$RESTART_MODE" in
	always) do_restart=1 ;;
	never) do_restart=0 ;;
	prompt)
		if [ -t 0 ]; then
			printf 'Restart the gitea container now? [Y/n] '
			read -r answer || answer="y"
			case "$answer" in
			[Nn]*) do_restart=0 ;;
			*) do_restart=1 ;;
			esac
		else
			do_restart=0
		fi
		;;
	esac
	if [ "$do_restart" = "1" ]; then
		log "restarting gitea..."
		(cd "$DOCKER_COMPOSE_DIR" && docker compose restart gitea)
	else
		log "not restarting."
	fi
}

if [ "$UNINSTALL" = "1" ]; then
	remove_managed
	fix_perms
	maybe_restart
	log "DC highlighter removed."
	exit 0
fi

for f in grammar.js gitea-hook.js; do
	[ -s "$ASSET_SRC_DIR/$f" ] || die "missing $ASSET_SRC_DIR/$f"
done
[ -s "$CORE_SRC" ] || die "missing $CORE_SRC (run download-vendor.sh first)"
[ -s "$CSS_SRC" ] || die "missing $CSS_SRC"

log "Gitea custom dir: $GITEA_DIR"
mkdir -p "$ASSET_DIR" "$TPL_DIR"

log "installing assets to $ASSET_DIR"
cp -f "$CORE_SRC" "$ASSET_DIR/prism-core.min.js"
cp -f "$CSS_SRC" "$ASSET_DIR/prism-gitea.css"
cp -f "$ASSET_SRC_DIR/grammar.js" "$ASSET_DIR/grammar.js"
cp -f "$ASSET_SRC_DIR/gitea-hook.js" "$ASSET_DIR/gitea-hook.js"

merge_footer
fix_perms
maybe_restart

log "==> done"
