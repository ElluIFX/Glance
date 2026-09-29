package main

import (
	"encoding/json"
	"errors"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"

	"github.com/richardlehane/siegfried"
	"golang.org/x/sys/windows"
)

const readBudget = 256 << 20

var errBudget = errors.New("read budget exceeded")

// Siegfried's external source interface avoids whole-file mappings and copies.
type boundedSource struct {
	file     *os.File
	size     int64
	mu       sync.Mutex
	read     int64
	exceeded bool
}

func (s *boundedSource) Read(p []byte) (int, error) {
	return 0, errors.New("sequential reads unsupported")
}
func (s *boundedSource) IsSlicer() bool { return true }
func (s *boundedSource) Size() int64    { return s.size }
func (s *boundedSource) Slice(off int64, length int) ([]byte, error) {
	if off < 0 || length < 0 || off > s.size {
		return nil, io.EOF
	}
	n := min(int64(length), s.size-off)
	s.mu.Lock()
	if n > readBudget-s.read {
		s.exceeded = true
		s.mu.Unlock()
		return nil, errBudget
	}
	s.read += n
	s.mu.Unlock()
	data := make([]byte, int(n))
	count, err := s.file.ReadAt(data, off)
	if err == nil && n < int64(length) {
		err = io.EOF
	}
	return data[:count], err
}
func (s *boundedSource) EofSlice(off int64, length int) ([]byte, error) {
	if off < 0 || off > s.size || length < 0 {
		return nil, io.EOF
	}
	start := s.size - off - int64(length)
	if start < 0 {
		return s.Slice(0, int(s.size-off))
	}
	return s.Slice(start, length)
}

type candidate struct {
	Name       string   `json:"name"`
	Version    string   `json:"version"`
	MIME       string   `json:"mime"`
	ID         string   `json:"id"`
	Basis      string   `json:"basis"`
	Extensions []string `json:"extensions"`
}
type response struct {
	Status     string      `json:"status"`
	Candidates []candidate `json:"candidates"`
}

func identify(path, directory string, ready func() error) response {
	result := response{Status: "failed"}
	sf, err := siegfried.Load(filepath.Join(directory, "default.sig"))
	if err != nil {
		return result
	}
	data, err := os.ReadFile(filepath.Join(directory, "formats.json"))
	if err != nil {
		return result
	}
	extensions := make(map[string][]string)
	if json.Unmarshal(data, &extensions) != nil {
		return result
	}
	if ready() != nil {
		return result
	}
	name, err := windows.UTF16PtrFromString(path)
	if err != nil {
		return result
	}
	handle, err := windows.CreateFile(name, windows.GENERIC_READ,
		windows.FILE_SHARE_READ|windows.FILE_SHARE_WRITE|windows.FILE_SHARE_DELETE,
		nil, windows.OPEN_EXISTING, windows.FILE_ATTRIBUTE_NORMAL, 0)
	if err != nil {
		return result
	}
	file := os.NewFile(uintptr(handle), path)
	defer file.Close()
	before, err := file.Stat()
	if err != nil || !before.Mode().IsRegular() {
		return result
	}
	if before.Size() == 0 {
		result.Status = "success"
		return result
	}
	source := &boundedSource{file: file, size: before.Size()}
	ids, err := sf.Identify(source, "", "")
	if source.exceeded {
		result.Status = "limited"
		return result
	}
	if err != nil {
		return result
	}
	after, err := file.Stat()
	current, currentErr := os.Stat(path)
	if err != nil || currentErr != nil || !os.SameFile(before, current) || before.Size() != after.Size() || !before.ModTime().Equal(after.ModTime()) || !before.ModTime().Equal(current.ModTime()) {
		return result
	}
	for _, id := range ids {
		values := id.Values()
		fields := sf.Fields()[0]
		row := make(map[string]string)
		for i, key := range fields {
			if i < len(values) {
				row[key] = values[i]
			}
		}
		if row["id"] == "" || row["id"] == "UNKNOWN" {
			continue
		}
		basis := row["basis"]
		if !strings.Contains(basis, "byte match") && !strings.Contains(basis, "container") && !strings.Contains(basis, "text match") && !strings.Contains(basis, "xml match") {
			continue
		}
		result.Candidates = append(result.Candidates, candidate{row["format"], row["version"], row["mime"], row["id"], basis, extensions[row["id"]]})
		if len(result.Candidates) == 16 {
			return response{Status: "limited"}
		}
	}
	result.Status = "success"
	return result
}

func main() {
	runtime.GOMAXPROCS(1)
	if len(os.Args) != 2 {
		os.Exit(2)
	}
	exe, err := os.Executable()
	if err != nil {
		os.Exit(2)
	}
	result := identify(os.Args[1], filepath.Dir(exe), func() error {
		_, err := io.WriteString(os.Stdout, "ready\n")
		return err
	})
	if json.NewEncoder(os.Stdout).Encode(result) != nil {
		os.Exit(3)
	}
}
