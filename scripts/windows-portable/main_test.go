package main

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
)

func TestCleanupRejectsUnrelatedProcessAndPreservesFiles(t *testing.T) {
	root, err := os.MkdirTemp("", "EZ4Connectpro-test-")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.RemoveAll(root) })
	sentinel := filepath.Join(root, "keep.txt")
	if err := os.WriteFile(sentinel, []byte("keep"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("EZ4CONNECTPRO_RUNTIME", root)
	if err := cleanupRuntime([]string{strconv.Itoa(os.Getpid()), root}); err == nil {
		t.Fatal("cleanup accepted a process outside its runtime")
	}
	if data, err := os.ReadFile(sentinel); err != nil || string(data) != "keep" {
		t.Fatal("rejected cleanup changed the directory")
	}
	if _, err := os.Stat(filepath.Join(root, ".cleanup-ready")); !os.IsNotExist(err) {
		t.Fatal("rejected cleanup claimed ownership")
	}
}

func TestCleanupRejectsInvalidArguments(t *testing.T) {
	root := t.TempDir()
	t.Setenv("EZ4CONNECTPRO_RUNTIME", root)
	for _, args := range [][]string{nil, {"0", root}, {"invalid", root}, {"1", "relative"}, {"1", root, "extra"}, {"1", filepath.Dir(root)}} {
		if err := cleanupRuntime(args); err == nil {
			t.Fatalf("accepted invalid cleanup: %v", args)
		}
	}
	if _, err := os.Stat(root); err != nil {
		t.Fatal("invalid cleanup removed a directory")
	}
}
