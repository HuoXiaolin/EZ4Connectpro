package main

import (
	"archive/zip"
	"bytes"
	_ "embed"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

//go:embed payload.zip
var payload []byte

func main() {
	if len(os.Args) > 1 && os.Args[1] == "--ez4-cleanup-runtime" {
		// Internal worker: never show an error dialog during shutdown.
		if err := cleanupRuntime(os.Args[2:]); err != nil {
			os.Exit(1)
		}
		return
	}
	if err := run(); err != nil {
		message, _ := syscall.UTF16PtrFromString("无法启动 EZ4Connectpro：\n" + err.Error())
		title, _ := syscall.UTF16PtrFromString("EZ4Connectpro")
		messageBox := syscall.NewLazyDLL("user32.dll").NewProc("MessageBoxW")
		messageBox.Call(0, uintptr(unsafe.Pointer(message)), uintptr(unsafe.Pointer(title)), 0x10)
		os.Exit(1)
	}
}

func run() error {
	launcher, err := os.Executable()
	if err != nil {
		return err
	}
	archive, err := zip.NewReader(bytes.NewReader(payload), int64(len(payload)))
	if err != nil {
		return err
	}
	runtime, err := os.MkdirTemp("", "EZ4Connectpro-")
	if err != nil {
		return err
	}
	cleanupOwned := true
	defer func() {
		if cleanupOwned {
			removeRuntime(runtime)
		}
	}()

	for _, file := range archive.File {
		if err := unpack(file, runtime); err != nil {
			return err
		}
	}
	app := exec.Command(filepath.Join(runtime, "EZ4Connectpro.exe"), os.Args[1:]...)
	app.Env = append(app.Environ(), "EZ4CONNECTPRO_LAUNCHER="+launcher, "EZ4CONNECTPRO_RUNTIME="+runtime)
	if err := app.Start(); err != nil {
		return err
	}
	finished := make(chan error, 1)
	go func() { finished <- app.Wait() }()
	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case err := <-finished:
			return err
		case <-ticker.C:
			ready, err := os.ReadFile(filepath.Join(runtime, ".cleanup-ready"))
			if err == nil && string(ready) == strconv.Itoa(app.Process.Pid) {
				cleanupOwned = false
				return nil
			}
		}
	}
}

func cleanupRuntime(args []string) error {
	if len(args) != 2 {
		return fmt.Errorf("invalid cleanup arguments")
	}
	pid, err := strconv.ParseUint(args[0], 10, 32)
	if err != nil || pid == 0 {
		return fmt.Errorf("invalid runtime owner")
	}
	root := filepath.Clean(args[1])
	if !filepath.IsAbs(root) || !strings.EqualFold(root, os.Getenv("EZ4CONNECTPRO_RUNTIME")) ||
		!strings.EqualFold(filepath.Dir(root), filepath.Clean(os.TempDir())) ||
		!strings.HasPrefix(filepath.Base(root), "EZ4Connectpro-") {
		return fmt.Errorf("invalid runtime directory")
	}
	info, err := os.Lstat(root)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return fmt.Errorf("runtime directory is unavailable")
	}
	// Hold a handle to the exact live owner, so PID reuse cannot affect shutdown.
	owner, err := syscall.OpenProcess(0x00100000|0x1000, false, uint32(pid)) // synchronize + query limited
	if err != nil {
		return err
	}
	defer syscall.CloseHandle(owner)
	var path [32768]uint16
	size := uint32(len(path))
	query := syscall.NewLazyDLL("kernel32.dll").NewProc("QueryFullProcessImageNameW")
	result, _, queryErr := query.Call(uintptr(owner), 0, uintptr(unsafe.Pointer(&path[0])), uintptr(unsafe.Pointer(&size)))
	if result == 0 {
		return queryErr
	}
	if !strings.EqualFold(syscall.UTF16ToString(path[:size]), filepath.Join(root, "EZ4Connectpro.exe")) {
		return fmt.Errorf("cleanup owner does not belong to this runtime")
	}
	if err := os.WriteFile(filepath.Join(root, ".cleanup-ready"), []byte(args[0]), 0600); err != nil {
		return err
	}
	if _, err := syscall.WaitForSingleObject(owner, syscall.INFINITE); err != nil {
		return err
	}
	return removeRuntime(root)
}

func removeRuntime(root string) error {
	var err error
	for attempt := 0; attempt < 50; attempt++ {
		if err = os.RemoveAll(root); err == nil {
			return nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return err
}

func unpack(file *zip.File, root string) error {
	relative := filepath.FromSlash(file.Name)
	if !filepath.IsLocal(relative) {
		return fmt.Errorf("invalid runtime file path: %s", file.Name)
	}
	target := filepath.Join(root, relative)
	if file.FileInfo().IsDir() {
		return os.MkdirAll(target, 0700)
	}
	if !file.Mode().IsRegular() {
		return fmt.Errorf("invalid runtime file type: %s", file.Name)
	}
	if err := os.MkdirAll(filepath.Dir(target), 0700); err != nil {
		return err
	}
	input, err := file.Open()
	if err != nil {
		return err
	}
	defer input.Close()
	output, err := os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(output, input)
	closeErr := output.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}
