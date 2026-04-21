# CI Debug Notes

## Intermittent proxy-related Go test failures

The replication tests below are currently treated as proxy-sensitive and flaky in GitHub Actions:

- `TestSlaveLostMaster`
- `TestSlowConsumerBug`
- `TestSlowConsumerBlocksIndefinitely`

The failure mode is intermittent rather than deterministic. A narrowed CI run may pass across the whole matrix and still not disprove the underlying issue.

## Current temporary workflow mode

The CI workflow temporarily narrows the Go integration step to make the flake easier to debug:

- package: `./integration/replication`
- test pattern: `^(TestSlaveLostMaster|TestSlowConsumerBug|TestSlowConsumerBlocksIndefinitely)$`
- `-parallel 1`
- `-count 1`
- `-timeout 300s`
- repeat the same narrowed proxy step multiple times per job
- `go test -v`

When one of these tests fails, the workflow also prints recent files from `tests/gocase/workspace` and uploads that workspace as an artifact.

## Why this temporary mode exists

- Reduce unrelated Go test noise while chasing an intermittent proxy failure.
- Make the failing scope small enough that GitHub Actions logs remain readable.
- Preserve workspace logs so the next reproduction has enough context for root-cause analysis.

## Recommended next debugging steps

- Enable `go test -v` for the narrowed proxy step when more test-level timing detail is needed.
- Repeat the narrowed proxy test step multiple times in CI or locally to increase reproduction odds.
- Inspect proxy helpers and cleanup timing in `tests/gocase/integration/replication` after a concrete reproduction is captured.
