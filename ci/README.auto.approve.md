# Flake bump auto approve

## Description

We add a final gitlab pipeline stage `bump`.
First job of this stage checks if a merge request contains only one commit which updates the `flake.lock` file.
If this condition is met the second job approve this merge request and automatically merge it.
The approval is done with two bot users (Gitlab Access Token).

## Setup auto approve

* Create two access tokens (aka bot user)
  * Gitlab -> Project -> Setttings -> Access Tokens
  * Create Token with:
    * Name: auto-approve-[12]
    * Role: Maintainer
    * Scopes: api
  * Copy Token into 1Password with a good name
* Add both bot's into Merge request rules
  * Gitlab -> Project -> Setttings -> Merge requests
  * in section: Merge request approvals
  * in rule: general rule
  * add both bots as eligible user
* Create two CI/CD Variables
  * Gitlab -> Project -> Setttings -> CI/CD -> Variables
  * add two variables (Key):
    * AUTO_APPROVE_1
    * AUTO_APPROVE_2
  * with visibility: Masked

## Pipeline

* add a last stage in your pipeline `bump`
* add two tasks:

```yaml
bump:auto-approve-check:
  stage: bump
  extends:
    - .nix_build_template
  rules:
    - if: $CI_PIPELINE_SOURCE == "merge_request_event"
      changes:
        - flake.lock
  script:
    # Determine the target branch of the merge request
    - TARGET_BRANCH=${CI_MERGE_REQUEST_TARGET_BRANCH_NAME:-$CI_DEFAULT_BRANCH}
    - echo "TARGET_BRANCH=$TARGET_BRANCH"
    # Lint commit messages in the range from target branch to current HEAD
    - nix run nixpkgs#gitlint -- --commits origin/$TARGET_BRANCH.. -C .gitlint_auto_approve

bump:auto-approve-merge:
  stage: bump
  needs: [ "bump:auto-approve-check" ]
  extends:
    - .nix_build_template
  rules:
    - if: $CI_PIPELINE_SOURCE == "merge_request_event"
      changes:
        - flake.lock
  script:
    - nix-shell -p curl --run './ci/auto_approve.sh'
```
