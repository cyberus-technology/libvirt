#!/usr/bin/env bash

# approval 1
curl --fail --request POST --header "PRIVATE-TOKEN: ${AUTO_APPROVE_1}" ${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/merge_requests/${CI_MERGE_REQUEST_IID}/approve

# approval 2
curl --fail --request POST --header "PRIVATE-TOKEN: ${AUTO_APPROVE_2}" ${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/merge_requests/${CI_MERGE_REQUEST_IID}/approve

# merge
curl --fail --request PUT --header "PRIVATE-TOKEN: ${AUTO_APPROVE_1}" --data "merge_when_pipeline_succeeds=true" ${CI_API_V4_URL}/projects/${CI_PROJECT_ID}/merge_requests/${CI_MERGE_REQUEST_IID}/merge
