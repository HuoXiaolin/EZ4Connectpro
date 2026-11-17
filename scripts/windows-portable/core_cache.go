package main

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

var coreFileNames = []string{"zju-connect.exe", "wintun.dll"}

func isCoreFile(name string) bool {
	for _, candidate := range coreFileNames {
		if name == candidate {
			return true
		}
	}
	return false
}

func coreCacheBase() (string, error) {
	root, err := os.UserCacheDir()
	if err != nil || !filepath.IsAbs(root) {
		return "", fmt.Errorf("无法定位本机连接核心目录")
	}
	return filepath.Join(root, "EZ4Connectpro", "cores"), nil
}

func coreBundle(archive *zip.Reader) (map[string][]byte, string, error) {
	files := make(map[string][]byte)
	for _, file := range archive.File {
		if !isCoreFile(file.Name) {
			continue
		}
		if _, exists := files[file.Name]; exists || !file.Mode().IsRegular() || file.UncompressedSize64 == 0 || file.UncompressedSize64 > 64<<20 {
			return nil, "", fmt.Errorf("无效的连接核心文件")
		}
		reader, err := file.Open()
		if err != nil {
			return nil, "", err
		}
		data, err := io.ReadAll(io.LimitReader(reader, 64<<20+1))
		reader.Close()
		if err != nil || len(data) > 64<<20 {
			return nil, "", fmt.Errorf("连接核心校验失败")
		}
		files[file.Name] = data
	}
	if len(files) != len(coreFileNames) {
		return nil, "", fmt.Errorf("安装包缺少连接核心或虚拟网卡库")
	}
	return files, coreBundleID(files), nil
}

func coreBundleID(files map[string][]byte) string {
	hash := sha256.New()
	for _, name := range coreFileNames {
		hash.Write([]byte(name + "\x00"))
		sum := sha256.Sum256(files[name])
		hash.Write(sum[:])
	}
	return hex.EncodeToString(hash.Sum(nil))
}

func regularPath(path string, directory bool) bool {
	info, err := os.Lstat(path)
	if err != nil || info.IsDir() != directory {
		return false
	}
	attributes, ok := info.Sys().(*syscall.Win32FileAttributeData)
	return ok && attributes.FileAttributes&syscall.FILE_ATTRIBUTE_REPARSE_POINT == 0 && (directory || info.Mode().IsRegular())
}

func ensureCacheBase(base string) error {
	if !filepath.IsAbs(base) {
		return fmt.Errorf("连接核心目录必须为绝对路径")
	}
	// Refuse junctions before writing executable files into a reusable directory.
	var parents []string
	for path := filepath.Clean(base); ; path = filepath.Dir(path) {
		parents = append(parents, path)
		if path == filepath.Dir(path) {
			break
		}
	}
	for i := len(parents) - 1; i >= 0; i-- {
		if _, err := os.Lstat(parents[i]); os.IsNotExist(err) {
			if err := os.Mkdir(parents[i], 0700); err != nil && !os.IsExist(err) {
				return err
			}
		}
		if !regularPath(parents[i], true) {
			return fmt.Errorf("连接核心目录包含链接或不可用路径")
		}
	}
	return nil
}

func openCoreHandle(path string, share uint32, create bool) (syscall.Handle, error) {
	p, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return 0, err
	}
	disposition := uint32(syscall.OPEN_EXISTING)
	access := uint32(syscall.GENERIC_READ)
	if create {
		disposition = syscall.OPEN_ALWAYS
		access |= syscall.GENERIC_WRITE
	}
	return syscall.CreateFile(p, access, share, nil, disposition, syscall.FILE_ATTRIBUTE_NORMAL|syscall.FILE_FLAG_OPEN_REPARSE_POINT, 0)
}

func lockCoreCache(base string) (func(), error) {
	path := filepath.Join(base, ".cache-lock")
	if _, err := os.Lstat(path); err == nil && !regularPath(path, false) {
		return nil, fmt.Errorf("无效的连接核心锁文件")
	}
	deadline := time.Now().Add(5 * time.Second)
	for {
		handle, err := openCoreHandle(path, 0, true)
		if err == nil {
			return func() { syscall.CloseHandle(handle) }, nil
		}
		if err != syscall.Errno(32) || time.Now().After(deadline) { // ERROR_SHARING_VIOLATION
			return nil, fmt.Errorf("连接核心正在更新，请稍后重试：%w", err)
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func validCachedCore(root, id string) bool {
	if !regularPath(root, true) || filepath.Base(root) != "core-"+id {
		return false
	}
	entries, err := os.ReadDir(root)
	if err != nil || len(entries) != len(coreFileNames) {
		return false
	}
	files := make(map[string][]byte)
	for _, name := range coreFileNames {
		path := filepath.Join(root, name)
		if !regularPath(path, false) {
			return false
		}
		info, err := os.Stat(path)
		if err != nil || info.Size() <= 0 || info.Size() > 64<<20 {
			return false
		}
		files[name], err = os.ReadFile(path)
		if err != nil {
			return false
		}
	}
	return coreBundleID(files) == id
}

func leaseCore(root string, exclusive bool) (func(), error) {
	var handles []syscall.Handle
	closeHandles := func() {
		for _, handle := range handles {
			syscall.CloseHandle(handle)
		}
	}
	share := uint32(syscall.FILE_SHARE_READ)
	if exclusive {
		share = 0
	}
	for _, name := range coreFileNames {
		handle, err := openCoreHandle(filepath.Join(root, name), share, false)
		if err != nil {
			closeHandles()
			return nil, err
		}
		handles = append(handles, handle)
	}
	return closeHandles, nil
}

func pruneCoreCaches(base, current string) {
	entries, _ := os.ReadDir(base)
	for _, entry := range entries {
		name := entry.Name()
		id := strings.TrimPrefix(name, "core-")
		decoded, err := hex.DecodeString(id)
		root := filepath.Join(base, name)
		if name == current || err != nil || len(decoded) != sha256.Size || !validCachedCore(root, id) {
			continue
		}
		release, err := leaseCore(root, true)
		if err != nil {
			continue // An older UI or VPN core still uses this bundle.
		}
		release()
		// Callers hold the cache lock; no new launcher can acquire this bundle.
		_ = os.RemoveAll(root)
	}
}

// Only the networking core and Wintun persist. Their content determines the path,
// so restarting the same build reuses the executable identity in firewall rules.
// Leases prevent replacing/removing files while a UI or its VPN cores are active.
func prepareCoreCache(archive *zip.Reader, base string) (string, func(), error) {
	files, id, err := coreBundle(archive)
	if err != nil {
		return "", nil, err
	}
	if err := ensureCacheBase(base); err != nil {
		return "", nil, err
	}
	unlock, err := lockCoreCache(base)
	if err != nil {
		return "", nil, err
	}
	defer unlock()
	root := filepath.Join(base, "core-"+id)
	if _, err := os.Lstat(root); os.IsNotExist(err) {
		stage, err := os.MkdirTemp(base, ".stage-")
		if err != nil {
			return "", nil, err
		}
		defer os.RemoveAll(stage)
		for _, name := range coreFileNames {
			if err := os.WriteFile(filepath.Join(stage, name), files[name], 0600); err != nil {
				return "", nil, err
			}
		}
		if err := os.Rename(stage, root); err != nil {
			return "", nil, err
		}
	}
	if !validCachedCore(root, id) {
		return "", nil, fmt.Errorf("连接核心缓存校验失败。请退出软件后移除该缓存目录再重试：%s", root)
	}
	release, err := leaseCore(root, false)
	if err != nil {
		return "", nil, fmt.Errorf("连接核心缓存被占用：%w", err)
	}
	pruneCoreCaches(base, filepath.Base(root))
	return filepath.Join(root, "zju-connect.exe"), release, nil
}
