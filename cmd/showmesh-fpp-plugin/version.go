package main

import (
	"flag"
	"fmt"
	"io"
	"time"

	"github.com/showmeshsystems/showmesh-fpp-plugin/internal/version"
)

// versionOutput is this program's own --output json shape for "version".
type versionOutput struct {
	Version   string `json:"version"`
	Commit    string `json:"commit"`
	BuildDate string `json:"buildDate"`
}

func cmdVersion(args []string, stdout, stderr io.Writer, _ func() time.Time) int {
	fs := flag.NewFlagSet("showmesh-fpp-plugin version", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var output string
	fs.StringVar(&output, "output", outputText, "output format: text|json")
	fs.Usage = func() {
		_, _ = fmt.Fprintln(stderr, "usage: showmesh-fpp-plugin version [flags]")
		fs.PrintDefaults()
	}
	if err := fs.Parse(args); err != nil {
		return flagParseExit(err)
	}
	if output != outputText && output != outputJSON {
		_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin version: invalid --output value %q: must be text or json\n", output)
		return exitUsage
	}

	if output == outputJSON {
		out := versionOutput{Version: version.Version, Commit: version.Commit, BuildDate: version.BuildDate}
		if err := printJSON(stdout, out); err != nil {
			_, _ = fmt.Fprintf(stderr, "showmesh-fpp-plugin version: %v\n", err)
			return exitLocalError
		}
		return exitOK
	}

	_, _ = fmt.Fprintf(stdout, "showmesh-fpp-plugin: %s\n", version.String())
	return exitOK
}
