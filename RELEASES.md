# Release operations

Builds run in `hernan-lc/obs-multi-rtmp`; the default destination is
`nglmercer/obs-multi-rtmp`. Merge this PR in the source repository, then sync its
workflow and script commits into the build repository before tagging. Existing
Windows, macOS, Ubuntu 24.04/26.04 and Flatpak jobs remain in use.

## Credentials and destination

In the **build repository**, configure Actions variable `RELEASE_REPOSITORY` to
`nglmercer/obs-multi-rtmp` (also the default). Set it to
`hernan-lc/obs-multi-rtmp` for local publication. Manual dispatch can override it.
Same-repository publication always uses `GITHUB_TOKEN` with job-scoped
`contents: write`. Cross-repository publication requires Actions secret
`RELEASE_TOKEN`: a fine-grained PAT selected for the destination repository with
**Contents: read and write**, or a valid GitHub App installation token granting
that permission. Metadata read is implicit. App tokens expire; arrange token
refresh before each run rather than storing a long-expired token. No App private
key or token minting is handled by this workflow.

Keep the existing macOS signing/notarization secrets if signed packages are
required. Protect tags and the default branch in the build repository; only
trusted maintainers should be able to dispatch releases or modify workflows.
Publishing runs only for tag pushes or manual dispatch from the default branch.
PRs run build/tests with read permission and never publish. Never supply
`RELEASE_TOKEN` to untrusted workflows. Repository Actions policy must allow the
pinned softprops v2.6.2 action and artifact actions. Ubuntu 26.04 and macOS 26
runner/Xcode availability remains an existing infrastructure requirement.

## Versions and tagging

Tags accept `[v]MAJOR.MINOR.PATCH[.BUILD]`, optionally suffixed with `-beta`,
`-beta1`, `-beta.1`, `-rc`, `-rc1`, or `-rc.1`. Beta/RC tags are prereleases.
Invalid tags fail explicitly before building. Buildspec package versions are
validated with the same grammar. Release and package versions are intentionally
independent: release `v0.0.3` may contain package version `0.7.5.0` to retain
compatibility with existing package metadata. Files are renamed, never repacked;
`release-metadata.json` and release notes record both versions and source SHA.
Update buildspec separately when the binary/package version should change.

Before pushing a tag to the build repository, push the **same tag and commit**
to the destination repository. Both lightweight and annotated tags are supported;
the workflow compares their resolved commit SHAs and refuses mismatches or
missing tags. It never creates or moves a destination tag automatically.
For example, after obtaining the merged source commit locally:

```sh
git tag v0.0.3 <merged-source-commit>
git push origin refs/tags/v0.0.3
git push https://github.com/hernan-lc/obs-multi-rtmp.git refs/tags/v0.0.3
```

Tag pushes automatically build all platforms and publish after manifest
validation. Required outputs are listed in `.github/scripts/release/manifest.json`:
Windows ZIP and EXE, macOS PKG, DEB for each Ubuntu distribution, and Flatpak.
Other supported package formats are collected when present. Debug symbols are
optional, named separately, and also checksummed. Conflicting normalized names,
empty files and missing required packages fail. The `validated-release-*` CI
artifact contains packages, debug symbols, metadata, checksums and notes.

## Recovery

From the build repository's default branch, dispatch **Release and recovery**:

- `version`: the existing source tag, including its original `v` spelling.
- `target_repository`: blank for configured destination; set explicitly for local fallback.
- `mode`: `preserve` by default. Matching existing assets stay intact. Any changed
  checksum fails before upload; choose `replace` only when intentionally repairing
  conflicting files. Unrelated existing assets are preserved in both modes.
- `recovery_run`: blank rebuilds the exact tagged commit; otherwise downloads
  artifacts from that successful trusted workflow run in the build repository.

For `v0.0.2`, run **38566601416** at source commit
`591460ae855fa3e283f2ac169c66951c54313705` had all required artifacts available
and passed local collection validation during implementation. Set `version` to
`v0.0.2`, `recovery_run` to `38566601416`, and the intended destination explicitly.
The destination must already have `v0.0.2` at that commit. Artifact retention may
expire; if download fails, leave the run ID blank to rebuild. Rebuilding uses
the tagged build scripts and dependencies; historical revisions may require
historical runner/toolchain availability. Recovery orchestration uses the current
trusted workflow's scripts, with buildspec read from the tagged revision.

Runs for the same destination/tag are serialized without cancellation. This lock
is repository-scoped: use one build repository as publisher for each destination.
Retries preserve matching assets; partial uploads fail final verification and
can be completed by rerunning. Every expected asset is downloaded through the
GitHub API and its SHA-256 verified after upload. Upload is not transactional:
a release may be visible during upload, or remain partial after a network failure;
the workflow fails and a retry repairs it. No successful cross-repository upload
was performed as part of implementing this PR.

Download `SHA256SUMS.txt` and all listed assets into one directory, then run:

```sh
sha256sum -c SHA256SUMS.txt
```

## Local validation

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_release*.py' -v
actionlint .github/workflows/push.yaml .github/workflows/release.yaml .github/workflows/build-project.yaml
git diff --check
```

The actionlint config declares the existing Ubuntu 26.04 runner label because
its current built-in runner catalog does not yet recognize it. Tests cover tag
syntax/prereleases, missing/empty/conflicting packages, checksums, authentication
failure, partial uploads, matching retries and invalid recovery provenance.
Actual platform builds and publication require GitHub runners and configured
credentials; mocked failure tests do not prove destination authorization.
