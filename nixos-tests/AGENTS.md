# AGENTS.md

This file supplements the repository root `AGENTS.md` for everything under
`nixos-tests/`.

Apply both files for changes in this subtree. If guidance conflicts, this file
takes precedence for files under `nixos-tests/`.

## Scope

This subtree contains the NixOS integration tests we use for libvirt and Cloud
Hypervisor development.

The basic test model is:

- the test driver starts one or two NixOS VM hosts: `controllerVM` and
  `computeVM`
- those hosts run libvirt and Cloud Hypervisor
- the guest under test usually runs inside Cloud Hypervisor as `testvm`
- some suites cover migration between `controllerVM` and `computeVM`

Read `README.md` before making non-trivial changes to test infrastructure,
inputs, or workflows.

## Where Changes Should Usually Go

- Add or adjust test scenarios in `tests/testsuite_*.py`.
- Put shared Python helpers in `test_helper/test_helper/` instead of
  duplicating logic across suites.
- Change common NixOS test infrastructure in `tests/libvirt-test.nix`,
  `tests/common.nix`, `modules/`, and `images/`.
- Change the test matrix and exported test attributes in `tests/default.nix`.
- Keep `README.md` and `docs/` in sync when commands, architecture, or test
  coverage change.

## Priorities

- Correctness, determinism, and debuggability are more important than reducing
  a few seconds of test runtime.
- Keep patches minimal, scoped, and directly relevant to the problem.
- Do not change test behavior unless required by the task or by a real bug.
- Avoid speculative refactors and broad infrastructure churn.

## Test Design Guidance

- Prefer existing helper functions over ad hoc shell snippets when equivalent
  helpers already exist.
- Prefer polling for an observable state change over fixed sleeps.
- If a fixed `sleep` is truly unavoidable, keep it minimal and explain why in
  the code or commit message.
- Make assertions explicit. Check the relevant VM, domain state, and side
  effects rather than relying on indirect behavior.
- When testing failure paths, assert the failure directly instead of accepting
  ambiguous command output.
- Keep in mind which layer a change affects: host test driver, `controllerVM`,
  `computeVM`, or the nested guest.

## Python Guidance

- Follow the existing `unittest`-based structure in the test suites.
- Preserve the current dual-import pattern used by the Python test files unless
  you are intentionally replacing it everywhere. It exists to support both Nix
  execution and IDE/linting workflows.
- Reuse `LibvirtTestsBase` and shared helpers such as `wait_for_ssh`,
  `wait_until_succeed`, `wait_until_fail`, `assert_domain_domstate`, `hotplug`,
  and related helpers before introducing new waiting or assertion primitives.
- Shared waiting logic belongs in `test_helper`, not repeated inline across
  test suites.

## Nix Guidance

- Preserve the current structure where test suites are exposed through
  `tests/default.nix` and instantiated via `tests/libvirt-test.nix`.
- Do not break the `passthru.no_port_forwarding` variant; it exists for CI and
  parallel test execution.
- Be careful with changes to flake inputs, overlays, and local path examples in
  `flake.nix`. They are used for local development against custom libvirt or
  Cloud Hypervisor builds.

## Validation

When you change this subtree, use the validation that matches the change:

- Run `nix flake check` for normal validation.
- For targeted test changes, run
  `nix run -L .#tests.x86_64-linux.<attribute>.driver`.
- For interactive debugging, use
  `nix run -L .#tests.x86_64-linux.<attribute>.driverInteractive`.
- If you change the Python helper library or Python test files, ensure the
  formatting, lint, and type checks still pass.

## Truthfulness

- Do not invent APIs, Nix attributes, VM behavior, or libvirt/Cloud Hypervisor
  capabilities.
- If you are uncertain whether a behavior is provided by the NixOS test driver,
  the helper library, or libvirt itself, verify it in the code before changing
  it.
