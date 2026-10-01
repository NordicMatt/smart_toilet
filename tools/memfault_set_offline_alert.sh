#!/bin/bash
# Set the Device Offline alert threshold.
#
#   tools/memfault_set_offline_alert.sh <seconds> [alert_id]
#   tools/memfault_set_offline_alert.sh 108000          # 30 h
#
# The threshold MUST comfortably exceed the connectivity duty cycle
# (CONFIG_APP_CONN_DRAIN_PERIOD_S). A healthy duty-cycled unit is
# deliberately silent for one full DRAIN period, so a threshold below that
# emails every project member on every sleep window. Sizing used so far:
#   always-on build   -> 15 min
#   1 h DRAIN period  -> 2.5 h  (9000 s)
#   24 h DRAIN period -> 30 h   (108000 s)
#
# API notes (learned the hard way):
#   - auth is the ORG token as the basic-auth PASSWORD with an EMPTY username
#   - notification_targets are handle STRINGS ("everyone"), not objects, even
#     though GET returns them as objects
#   - notify_on_incident_start is required on write
#   - the delay must be a whole multiple of 15 min (900 s)
set -euo pipefail

SECONDS_THRESHOLD="${1:?usage: $0 <seconds> [alert_id] [devices_expr]   e.g. 86400}"
ALERT_ID="${2:-21563}"
# Device scope. `true` = every device in the project (the default). Anything
# else is a filter expression in the same dialect device_search uses, e.g.
#   "cohort != 'bench'"   to keep bench hardware out of fleet paging.
DEVICES_EXPR="${3:-true}"

if (( SECONDS_THRESHOLD % 900 != 0 )); then
	echo "Threshold must be a multiple of 900 s (15 min); got $SECONDS_THRESHOLD" >&2
	exit 1
fi

ORG=$(tr -d '\n' < ~/.memfault_org_slug)
PROJ=$(tr -d '\n' < ~/.memfault_project_slug)
TOKEN=$(tr -d '\n' < ~/.memfault_org_token)

HOURS=$(python3 -c "print(f'{$SECONDS_THRESHOLD/3600:g}')")

BODY=$(python3 - "$SECONDS_THRESHOLD" "$HOURS" "$DEVICES_EXPR" <<'PY'
import json, sys
secs, hours, expr = int(sys.argv[1]), sys.argv[2], sys.argv[3]
scope = "" if expr == "true" else f" Scope: {expr}."
body = {
    "title": f"Toilet offline > {hours} h (duty-cycle aware)",
    "description": (
        f"A smart-toilet unit has not checked in for over {hours} hours. "
        "Threshold is sized for the low-power duty-cycled connectivity: a "
        "healthy sleeping device is deliberately silent for one full DRAIN "
        "period. Do not lower this below CONFIG_APP_CONN_DRAIN_PERIOD_S or "
        "every sleep window pages the whole team." + scope
    ),
    "incident_start_delay_seconds": secs,
    "notify_on_incident_start": True,
    "notify_on_incident_end": True,
    "notification_targets": ["everyone"],
    "devices_expr": True if expr == "true" else expr,
}
print(json.dumps(body))
PY
)

echo "Setting alert $ALERT_ID -> ${SECONDS_THRESHOLD}s (${HOURS} h) (org=$ORG project=$PROJ)"

curl -sS -X PATCH \
	-u ":$TOKEN" \
	-H "Content-Type: application/json" \
	-d "$BODY" \
	"https://api.memfault.com/api/v0/organizations/$ORG/projects/$PROJ/alerts/$ALERT_ID" \
	| python3 -m json.tool

echo
echo "Verify incident_start_delay_seconds reads $SECONDS_THRESHOLD above."
