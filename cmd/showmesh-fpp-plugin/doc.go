// Command showmesh-fpp-plugin is a dedicated operational binary that runs
// ON an FPP host, invoked by FPP's own command mechanism (a script command
// registered through commands/descriptions.json — per RES-015 section 7.2),
// whose job is to fire a ShowMesh macro and to make what
// happened legible on the FPP host itself, without needing the coordinator
// to be reachable to read that record.
//
// This binary's runtime behaviour (credential handling, classification of
// a refusal versus an outage, the local status record, the buffered-
// failure report) is specific to running unattended on an FPP host.
//
// This program is the API's independent client: it may never import a
// coordinator package, enforced by importgraph_test.go. It decodes the
// wire contract on its own terms so a rename on the server side breaks
// this build rather than silently drifting.
//
// # Commands
//
//	showmesh-fpp-plugin run <macroId>   fire a macro run
//	showmesh-fpp-plugin status          print the local status record
//	showmesh-fpp-plugin version         print this binary's version
//
// # The credential
//
// The credential is read from a mode-0600 file this plugin owns
// (/etc/showmesh-fpp-plugin/credential, fixed — see config.go's
// credentialDir) and is never a command argument, a URL query parameter,
// or set in a child process's environment.
//
// That fixed location is deliberate and survived a correction: an earlier
// version of this plugin put the credential under FPP's own config
// directory (<config-dir>/credential, alongside config.json and this
// plugin's other state). FPP's config directory is not a safe place for a
// secret, and this was checked against the running bench FPP's own PHP
// source and by driving it, not assumed:
//
//   - GET /api/configfile/** resolves a URL-supplied filename against the
//     config directory and serves it with NO allowlist and NO
//     authentication. A file one level down in a subdirectory returned
//     200 with an empty body in that check, because the handler reads
//     only the first path segment and lists a directory rather than
//     serving a file inside it — that is a routing quirk, not a guard,
//     and one FPP refactor away from serving the file.
//   - POST /api/configfile/** (UploadConfigFile) creates subdirectories
//     from a path containing a slash, also unauthenticated — so anyone
//     who can reach the FPP host's HTTP port can write into that
//     directory: overwrite this plugin's credential, or forge
//     status.json, the file whose entire purpose is to be an honest
//     local record independent of the coordinator.
//   - FPP's backup redaction is an exact key-name match list
//     (emailpass/password/secret on the refs this project has checked),
//     so a file literally named "credential" is not redacted and would
//     be included in a plaintext backup download.
//
// /etc/showmesh-fpp-plugin sits outside FPP's web root, outside the media
// tree entirely (so it is also outside $MEDIADIR-derived state — see
// config.go's separate resolveConfigDir), outside the plugin's own git
// checkout (and therefore outside `git clean -fd`, which FPP 9.x runs as
// an upgrade fallback and would otherwise delete an untracked credential),
// and outside every one of the exposures above. Deliberately NOT
// configurable by flag or environment variable in production — see
// config.go's credentialDirOverride doc comment for why a test-only Go
// package variable, not a runtime input, is this plugin's only override
// mechanism.
//
// A second, independent reason for the fixed file mode still applies on
// top of the location: FPP publishes every command execution, with its
// arguments, in cleartext to MQTT "command/run", so a token passed as a
// command argument would be broadcast on every invocation of this
// program — a defect this program's own design (the credential is never
// an argument, at any location) makes structurally impossible, not merely
// discouraged. Refusing to run when the credential file's mode is not
// exactly 0600 is this program's own defence against a defect that would
// otherwise be invisible until read off the host or the broker.
//
// # Where the packaging lives
//
// This directory temporarily holds the Go runtime pending extraction to
// showmesh-fpp-plugin. The approved target has three repositories: showmesh
// owns coordinator surfaces, fpp-showmesh owns thin Plugin Manager packaging,
// and showmesh-fpp-plugin owns this Go helper plus a separate C++ brightness
// component, tests and release artifacts. The C++ component does not replace
// this script command. Neither the extraction nor the C++ component exists in
// this checkout yet. Release artifacts stay unpublished until the first
// real-host install (RES-018).
//
// # What this program does NOT prove
//
// This program is developed and tested against a bench FPP instance, never
// against a real installation. That licenses the plugin's own mechanism —
// how it classifies a response, how it writes its local record, how it
// buffers a degraded outcome — and says nothing about on-host install,
// filesystem permissions, packaging, or cross-version compatibility on a
// real FPP host. No comment, log line, or string in this package claims
// otherwise.
package main
