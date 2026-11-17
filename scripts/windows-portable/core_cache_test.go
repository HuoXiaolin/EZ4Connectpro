package main

import (
	"archive/zip"
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
)

func TestCoreCacheAllowsExecutionWhileLeased(t *testing.T) {
	program, err := os.ReadFile(filepath.Join(os.Getenv("SystemRoot"), "System32", "cmd.exe"))
	if err != nil {
		t.Fatal(err)
	}
	var data bytes.Buffer
	writer := zip.NewWriter(&data)
	for _, name := range coreFileNames {
		file, err := writer.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		contents := program
		if name == "wintun.dll" {
			contents = []byte("unused driver fixture")
		}
		if _, err := file.Write(contents); err != nil {
			t.Fatal(err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	archive, err := zip.NewReader(bytes.NewReader(data.Bytes()), int64(data.Len()))
	if err != nil {
		t.Fatal(err)
	}
	path, release, err := prepareCoreCache(archive, filepath.Join(t.TempDir(), "cores"))
	if err != nil {
		t.Fatal(err)
	}
	defer release()
	if err := exec.Command(path, "/c", "exit", "0").Run(); err != nil {
		t.Fatal(err)
	}
}

func fixtureCore(t *testing.T, version string) *zip.Reader {
	t.Helper()
	var data bytes.Buffer
	writer := zip.NewWriter(&data)
	for _, name := range append([]string{"EZ4Connectpro.exe"}, coreFileNames...) {
		file, err := writer.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := file.Write([]byte(name + version)); err != nil {
			t.Fatal(err)
		}
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	archive, err := zip.NewReader(bytes.NewReader(data.Bytes()), int64(data.Len()))
	if err != nil {
		t.Fatal(err)
	}
	return archive
}

func TestCoreCacheStableAcrossRestartAndConcurrentLaunch(t *testing.T) {
	base := filepath.Join(t.TempDir(), "cores")
	archive := fixtureCore(t, "one")
	first, release, err := prepareCoreCache(archive, base)
	if err != nil {
		t.Fatal(err)
	}
	info, _ := os.Stat(first)
	release()
	second, release, err := prepareCoreCache(archive, base)
	if err != nil {
		t.Fatal(err)
	}
	defer release()
	current, _ := os.Stat(second)
	if first != second || !info.ModTime().Equal(current.ModTime()) {
		t.Fatal("restart changed or rewrote the core executable")
	}
	var group sync.WaitGroup
	for i := 0; i < 4; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			path, closeLease, err := prepareCoreCache(archive, base)
			if err != nil {
				t.Error(err)
				return
			}
			defer closeLease()
			if path != first {
				t.Error("concurrent launch changed the core path")
			}
		}()
	}
	group.Wait()
	if err := os.WriteFile(first, []byte("replace active executable"), 0600); err == nil {
		t.Fatal("active core lease allowed replacement")
	}
}

func TestCoreCacheUpgradePreservesActiveAndUnknownFiles(t *testing.T) {
	base := filepath.Join(t.TempDir(), "cores")
	old, releaseOld, err := prepareCoreCache(fixtureCore(t, "one"), base)
	if err != nil {
		t.Fatal(err)
	}
	unknown := filepath.Join(base, "user-file.txt")
	os.WriteFile(unknown, []byte("keep"), 0600)
	archive := fixtureCore(t, "two")
	current, releaseCurrent, err := prepareCoreCache(archive, base)
	if err != nil {
		releaseOld()
		t.Fatal(err)
	}
	defer releaseCurrent()
	if old == current {
		t.Fatal("changed core reused old executable identity")
	}
	if _, err := os.Stat(old); err != nil {
		t.Fatal("upgrade removed a live older core")
	}
	releaseOld()
	_, release, err := prepareCoreCache(archive, base)
	if err != nil {
		t.Fatal(err)
	}
	release()
	if _, err := os.Stat(filepath.Dir(old)); !os.IsNotExist(err) {
		t.Fatal("unused old core bundle was not cleaned")
	}
	if data, err := os.ReadFile(unknown); err != nil || string(data) != "keep" {
		t.Fatal("cleanup changed unrelated files")
	}
}

func TestCoreCacheRejectsCorruptionAndExtraFiles(t *testing.T) {
	for _, mode := range []string{"corrupt", "extra"} {
		t.Run(mode, func(t *testing.T) {
			base := filepath.Join(t.TempDir(), "cores")
			archive := fixtureCore(t, "one")
			path, release, err := prepareCoreCache(archive, base)
			if err != nil {
				t.Fatal(err)
			}
			release()
			if mode == "corrupt" {
				os.WriteFile(path, []byte("changed"), 0600)
			} else {
				os.WriteFile(filepath.Join(filepath.Dir(path), "keep.txt"), []byte("keep"), 0600)
			}
			if _, release, err := prepareCoreCache(archive, base); err == nil {
				release()
				t.Fatal("unverified executable bundle was accepted")
			}
			if _, err := os.Stat(path); err != nil {
				t.Fatal("rejection destroyed the bundle")
			}
		})
	}
}
