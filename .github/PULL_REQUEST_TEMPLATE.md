## What changes

<!-- The observable behavior that changed, in a sentence or two. -->

## Why

<!-- Related issue, if any: Closes #123 -->

## How to test

<!--
Steps to verify by hand. This matters in UI and hardware code, where an
automated test cannot reach.
-->

## Checklist

- [ ] `make check` passes (builds **and** runs the suite)
- [ ] Behavior changes have a test; bug fixes have a test that failed before
- [ ] New comments are in English; UI text and log messages are in Portuguese
- [ ] No build flag was duplicated outside `flags.mk`
- [ ] No new path can break through the light floor
