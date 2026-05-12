#!/usr/bin/env bash

TARGET_BRANCH=${CI_MERGE_REQUEST_TARGET_BRANCH_NAME:-$CI_DEFAULT_BRANCH}
echo "TARGET_BRANCH=$TARGET_BRANCH"
nix run nixpkgs#gitlint -- --commits origin/$TARGET_BRANCH.. -C .gitlint_auto_approve

ELIGIBLE_AUTO_APPROVE=$?

if [ $ELIGIBLE_AUTO_APPROVE -eq 0 ]; then
  echo "this merge request is eligible for a flake bump auto approve and merge"

  # approval 1
  curl --fail --request POST --header "PRIVATE-TOKEN: ${AUTO_APPROVE_1}" ${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/merge_requests/${CI_MERGE_REQUEST_IID}/approve

  # approval 2
  curl --fail --request POST --header "PRIVATE-TOKEN: ${AUTO_APPROVE_2}" ${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/merge_requests/${CI_MERGE_REQUEST_IID}/approve

  # merge
  curl --fail --request PUT --header "PRIVATE-TOKEN: ${AUTO_APPROVE_1}" --data "merge_when_pipeline_succeeds=true" ${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/merge_requests/${CI_MERGE_REQUEST_IID}/merge

else
  echo "this merge request will not be automatically approved."
fi

exit 0
