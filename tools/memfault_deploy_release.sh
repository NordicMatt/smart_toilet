#!/bin/bash
# Publish an already-uploaded smart-toilet release to a cohort.
#
#   tools/memfault_deploy_release.sh <version> <cohort>
#   tools/memfault_deploy_release.sh 2.2.0+0 toilet-2-only
#
# Upload the artifacts FIRST (these work with the plain org token):
#   memfault ... upload-ota-payload  --software-version <v> build_lp/app.signed.bin
#   memfault ... upload-mcu-symbols  --software-version <v> build_lp/app/zephyr/zephyr.elf
#
# deploy-release does NOT accept --org-token: it 401s with
# "This resource requires log-in". It needs USER auth, i.e. --email plus the
# user API key in ~/.memfault_manager_token passed as --password.
#
# Credentials are read from the ~/.memfault_* dotfiles so no secret ever
# lands on the command line or in shell history.
set -euo pipefail

VERSION="${1:?usage: $0 <version> <cohort>   e.g. 2.2.0+0 toilet-2-only}"
COHORT="${2:?usage: $0 <version> <cohort>   e.g. 2.2.0+0 toilet-2-only}"

CLI="${MEMFAULT_CLI:-$HOME/.memfault-venv/bin/memfault}"
# The Memfault account is the Nordic work address, NOT the git/gmail identity.
EMAIL="${MEMFAULT_EMAIL:-matthew.heins@nordicsemi.no}"

[ -x "$CLI" ] || { echo "memfault CLI not found at $CLI" >&2; exit 1; }

ORG=$(tr -d '\n' < ~/.memfault_org_slug)
PROJ=$(tr -d '\n' < ~/.memfault_project_slug)
MTOKEN=$(tr -d '\n' < ~/.memfault_manager_token)

echo "Deploying $VERSION to cohort '$COHORT' (org=$ORG project=$PROJ)"

# NOTE: FOTA delivery is gated by Fleet Sampling. A device that is not in
# Developer Mode (or covered by a sampling profile that enables OTA) gets a
# 204 from the OTA endpoint no matter what is deployed here.
"$CLI" \
	--email "$EMAIL" \
	--password "$MTOKEN" \
	--org "$ORG" \
	--project "$PROJ" \
	deploy-release \
	--release-version "$VERSION" \
	--cohort "$COHORT"

echo "Deployed $VERSION -> $COHORT"
