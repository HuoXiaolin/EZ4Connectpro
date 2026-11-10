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
	"syscall"
	"unsafe"
)

//go:embed payload.zip
var payload []byte

func main() {
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
	defer os.RemoveAll(runtime)

	for _, file := range archive.File {
		if err := unpack(file, runtime); err != nil {
			return err
		}
	}
	app := exec.Command(filepath.Join(runtime, "EZ4Connectpro.exe"), os.Args[1:]...)
	app.Env = append(app.Environ(), "EZ4CONNECTPRO_LAUNCHER="+launcher)
	return app.Run()
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
