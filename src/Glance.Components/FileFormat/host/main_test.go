package main

import (
	"bytes"
	"errors"
	"io"
	"os"
	"testing"
)

func TestBoundedRandomAccess(t *testing.T) {
	f, err := os.CreateTemp(t.TempDir(), "source")
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	if _, err = f.Write([]byte("0123456789")); err != nil {
		t.Fatal(err)
	}
	s := &boundedSource{file: f, size: 10}
	data, err := s.EofSlice(2, 3)
	if err != nil || !bytes.Equal(data, []byte("567")) {
		t.Fatalf("tail: %q %v", data, err)
	}
	data, err = s.Slice(8, 4)
	if !errors.Is(err, io.EOF) || !bytes.Equal(data, []byte("89")) {
		t.Fatalf("partial: %q %v", data, err)
	}
	s.read = readBudget - 1
	if _, err = s.Slice(0, 2); !errors.Is(err, errBudget) || !s.exceeded {
		t.Fatal("read budget was not enforced")
	}
	if s.Size() != 10 {
		t.Fatal("logical file size changed")
	}
}
