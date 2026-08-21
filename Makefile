MODULE   := github.com/showmeshsystems/showmesh-fpp-plugin
CMD      := ./cmd/showmesh-fpp-plugin
BIN_DIR  := ./bin
BIN      := $(BIN_DIR)/showmesh-fpp-plugin

VERSION    ?= dev
COMMIT     := $(shell git rev-parse --short HEAD 2>/dev/null || echo none)
BUILD_DATE := $(shell date -u +%Y-%m-%dT%H:%M:%SZ)

GOLANGCI_LINT_VERSION ?= v2.6.2

LDFLAGS := -X $(MODULE)/internal/version.Version=$(VERSION) \
           -X $(MODULE)/internal/version.Commit=$(COMMIT) \
           -X $(MODULE)/internal/version.BuildDate=$(BUILD_DATE)

.PHONY: build
build:
	mkdir -p $(BIN_DIR)
	CGO_ENABLED=0 go build -ldflags "$(LDFLAGS)" -o $(BIN) $(CMD)

.PHONY: test
test:
	go test ./...

.PHONY: test-race
test-race:
	go test -race ./...

.PHONY: vet
vet:
	go vet ./...

.PHONY: fmt
fmt:
	gofmt -w .

.PHONY: fmt-check
fmt-check:
	@unformatted=$$(gofmt -l .); \
	if [ -n "$$unformatted" ]; then \
		echo "gofmt needed on:"; echo "$$unformatted"; exit 1; \
	fi

.PHONY: lint
lint:
	@if command -v golangci-lint >/dev/null 2>&1; then \
		echo "using installed golangci-lint"; \
		golangci-lint run ./...; \
	else \
		echo "golangci-lint not on PATH; using go run github.com/golangci/golangci-lint/v2/cmd/golangci-lint@$(GOLANGCI_LINT_VERSION)"; \
		go run github.com/golangci/golangci-lint/v2/cmd/golangci-lint@$(GOLANGCI_LINT_VERSION) run ./...; \
	fi

.PHONY: native
native:
	$(MAKE) -C native all

.PHONY: native-test
native-test:
	$(MAKE) -C native check-host-neutral
	$(MAKE) -C native test

.PHONY: check
check: fmt-check vet lint test native-test

.PHONY: clean
clean:
	rm -rf $(BIN_DIR) $(DIST)
	$(MAKE) -C native clean

# ---------------------------------------------------------------------------
# Release artifacts
#
# FPP hosts carry no Go toolchain, so this binary is installed as a
# prebuilt, sha256-verified static binary. The artifact contract is pinned
# so the packaging repository can fetch and verify against it without a
# coordinated second change:
#   tag:   fpp-plugin-v<VERSION>
#   asset: showmesh-fpp-plugin_<VERSION>_linux_<ARCH>.tar.gz, ARCH in
#          {amd64, arm64, armv7}
#   sums:  showmesh-fpp-plugin_<VERSION>_SHA256SUMS, standard sha256sum
#          format ("<hex>  <filename>")
#
# Nothing here publishes anything. Candidate artifacts stay private until
# the first real-host install gate passes and the owner approves
# publication.
# ---------------------------------------------------------------------------
DIST         := ./dist
DIST_VERSION ?= $(VERSION)

# The pinned commit's own timestamp, not $(BUILD_DATE)'s wall clock, which
# by construction differs on every invocation and would make two builds of
# one commit non-reproducible for no reason connected to the source.
DIST_COMMIT_DATE := $(shell git show -s --format=%cI HEAD 2>/dev/null || echo unknown)
DIST_LDFLAGS := -X $(MODULE)/internal/version.Version=$(DIST_VERSION) \
                -X $(MODULE)/internal/version.Commit=$(COMMIT) \
                -X $(MODULE)/internal/version.BuildDate=$(DIST_COMMIT_DATE)

# The determinism flags below (--sort, --owner, --group, --numeric-owner,
# --mtime) are GNU tar's, not macOS bsdtar's. TAR_IS_GNU gates on that at
# parse time so a machine with neither still gets a correct tarball, just
# not a byte-reproducible one; CI always has GNU tar.
TAR := $(shell command -v gtar 2>/dev/null || command -v tar 2>/dev/null)
TAR_IS_GNU := $(shell $(TAR) --version 2>/dev/null | grep -qi 'gnu tar' && echo yes)

# $(1) GOARCH, $(2) extra build env, $(3) the asset filename's ARCH label
# (not always $(1): armv7's GOARCH is "arm"), $(4) output directory.
#
# -trimpath makes the binary reproducible; it does not touch the archive.
# A plain `tar -czf` still records each entry's mtime, uid, gid, and
# owner/group NAME, which a GNU tar running as root on the installing host
# would apply to the extracted file, and gzip's own header carries a
# timestamp. Piping to `gzip -n` strips those; tar's built-in -z cannot.
define build_and_package
	mkdir -p $(4)
	rm -f $(4)/showmesh-fpp-plugin
	GOOS=linux GOARCH=$(1) $(2) CGO_ENABLED=0 go build -trimpath -ldflags "$(DIST_LDFLAGS)" -o $(4)/showmesh-fpp-plugin $(CMD)
	chmod 0755 $(4)/showmesh-fpp-plugin
	if [ "$(TAR_IS_GNU)" = "yes" ]; then \
		$(TAR) --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' -C $(4) -cf - showmesh-fpp-plugin | gzip -n -9 > $(4)/showmesh-fpp-plugin_$(DIST_VERSION)_linux_$(3).tar.gz; \
	else \
		echo "WARNING: GNU tar not found on PATH (checked gtar, tar); building linux_$(3)'s tarball WITHOUT deterministic mtime/owner/group stripping. It is still correct, but will not reproduce byte-for-byte across two local runs. Install GNU tar (e.g. 'brew install gnu-tar' on macOS) to get that locally." >&2; \
		tar -C $(4) -czf $(4)/showmesh-fpp-plugin_$(DIST_VERSION)_linux_$(3).tar.gz showmesh-fpp-plugin; \
	fi
	rm -f $(4)/showmesh-fpp-plugin
endef

.PHONY: release-amd64
release-amd64:
	$(call build_and_package,amd64,,amd64,$(DIST))

.PHONY: release-arm64
release-arm64:
	$(call build_and_package,arm64,,arm64,$(DIST))

.PHONY: release-armv7
release-armv7:
	$(call build_and_package,arm,GOARM=7,armv7,$(DIST))

# The resident component ships as architecture-independent source, not a
# binary: FPP 10 replaces the HTTP framework and revamps the plugin
# manager, so the adapter is compiled on the host against that host's own
# installed headers. The bundle carries the host-neutral core, both
# adapters, their build files, the core tests, the version pins, and the
# license. The tests travel with it so an installer can run them on the
# host after compiling, as a validation step before activation.
NATIVE_BUNDLE := showmesh-fpp-plugin-native_$(DIST_VERSION).tar.gz

.PHONY: release-native-bundle
release-native-bundle:
	mkdir -p $(DIST)
	rm -f $(DIST)/$(NATIVE_BUNDLE)
	@if [ "$(TAR_IS_GNU)" = "yes" ]; then \
		$(TAR) --sort=name --owner=0 --group=0 --numeric-owner --mtime='@0' \
			--exclude='build' --exclude='.DS_Store' \
			-cf - LICENSE native/include native/src native/tests native/adapters native/Makefile \
			| gzip -n -9 > $(DIST)/$(NATIVE_BUNDLE); \
	else \
		echo "WARNING: GNU tar not found on PATH; the native source bundle will not reproduce byte-for-byte across two local runs." >&2; \
		tar --exclude='build' --exclude='.DS_Store' -czf $(DIST)/$(NATIVE_BUNDLE) LICENSE native/include native/src native/tests native/adapters native/Makefile; \
	fi
	@echo "release-native-bundle: wrote $(DIST)/$(NATIVE_BUNDLE)"

# Builds every artifact, writes the checksums file the pinned contract
# names, then verifies it against what was just produced, on every
# invocation rather than as a trusted one-time claim.
.PHONY: release
release: release-amd64 release-arm64 release-armv7 release-native-bundle
	cd $(DIST) && sha256sum showmesh-fpp-plugin_$(DIST_VERSION)_linux_*.tar.gz $(NATIVE_BUNDLE) > showmesh-fpp-plugin_$(DIST_VERSION)_SHA256SUMS
	cd $(DIST) && sha256sum -c showmesh-fpp-plugin_$(DIST_VERSION)_SHA256SUMS
	$(MAKE) release-manifest
	@echo "release: built and self-verified $(DIST)/showmesh-fpp-plugin_$(DIST_VERSION)_SHA256SUMS"

# The manifest is what the packaging repository turns into its committed
# lock file. It names every artifact, its size, and its SHA-256, so an
# installer verifies against a hash committed beside the install script
# rather than one fetched from the same mutable location as the artifact.
# It carries no timestamp, so two builds of one commit produce the same
# manifest.
.PHONY: release-manifest
release-manifest:
	@scripts/write-release-manifest.sh "$(DIST)" "$(DIST_VERSION)" "$(COMMIT)" > $(DIST)/release-manifest.json
	@scripts/verify-release-manifest.sh "$(DIST)" "$(DIST_VERSION)"

# The stronger claim `release`'s own sha256sum -c cannot make: two
# independent builds of the same commit produce byte-identical TARBALLS,
# not merely a manifest matching this run's own output. One platform is
# enough to prove the mechanism; the other two share the build shape.
.PHONY: verify-reproducible
verify-reproducible:
	rm -rf $(DIST)/.reproducible-a $(DIST)/.reproducible-b
	$(call build_and_package,amd64,,amd64,$(DIST)/.reproducible-a)
	$(call build_and_package,amd64,,amd64,$(DIST)/.reproducible-b)
	@if [ "$(TAR_IS_GNU)" != "yes" ]; then \
		echo "verify-reproducible: SKIPPED the byte-for-byte comparison: no GNU tar on PATH, so neither tarball was built deterministically and comparing them would only prove that, not reproducibility" >&2; \
		exit 1; \
	fi
	@if ! cmp -s $(DIST)/.reproducible-a/showmesh-fpp-plugin_$(DIST_VERSION)_linux_amd64.tar.gz $(DIST)/.reproducible-b/showmesh-fpp-plugin_$(DIST_VERSION)_linux_amd64.tar.gz; then \
		echo "two independent builds of the same commit produced DIFFERENT tarballs; the release artifact is not reproducible"; \
		exit 1; \
	fi
	rm -rf $(DIST)/.reproducible-a $(DIST)/.reproducible-b
	@echo "verify-reproducible: OK, two independent builds produced byte-identical tarballs"
