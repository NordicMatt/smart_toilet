#!/bin/bash
# Move a device into a cohort, creating the cohort if it does not exist.
#
#   tools/memfault_set_cohort.sh <device_serial> <cohort_slug> [nickname]
#   tools/memfault_set_cohort.sh 6C48F6592F87DC2A bench "Bench DK"
#
# Cohorts are how per-unit FOTA is targeted (see tools/memfault_deploy_release.sh).
# A device left in `default` receives whatever is deployed to the main fleet --
# which is exactly what you do NOT want for a bench board carrying
# experimental builds.
#
# Reads credentials from the ~/.memfault_* dotfiles; nothing sensitive lands
# on the command line.
set -uo pipefail

SERIAL="${1:?usage: $0 <device_serial> <cohort_slug> [nickname]}"
COHORT="${2:?usage: $0 <device_serial> <cohort_slug> [nickname]}"
NICKNAME="${3:-}"

ORG=$(tr -d '\n' < ~/.memfault_org_slug)
PROJ=$(tr -d '\n' < ~/.memfault_project_slug)
TOKEN=$(tr -d '\n' < ~/.memfault_org_token)
BASE="https://api.memfault.com/api/v0/organizations/$ORG/projects/$PROJ"

echo "=== creating cohort '$COHORT' (ok if it already exists) ==="
curl -sS -X POST -u ":$TOKEN" \
	-H "Content-Type: application/json" \
	-d "{\"name\": \"$COHORT\", \"slug\": \"$COHORT\"}" \
	"$BASE/cohorts" | python3 -m json.tool 2>/dev/null | head -12

BODY="{\"cohort\": \"$COHORT\""
if [ -n "$NICKNAME" ]; then
	BODY="$BODY, \"nickname\": \"$NICKNAME\""
fi
BODY="$BODY}"

echo
echo "=== moving $SERIAL -> $COHORT ==="
curl -sS -X PATCH -u ":$TOKEN" \
	-H "Content-Type: application/json" \
	-d "$BODY" \
	"$BASE/devices/$SERIAL" | python3 -c "
import sys, json
d = json.load(sys.stdin).get('data', {})
print('serial :', d.get('device_serial'))
print('nick   :', d.get('nickname'))
print('cohort :', (d.get('cohort') or {}).get('slug'))
"
