package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"os"
	"testing"
)

// buildMinimalHPROF creates a minimal valid-ish HPROF binary for testing
func buildMinimalHPROF() []byte {
	var buf bytes.Buffer

	// Header: "JAVA PROFILE 1.0.2\0" + idSize (4 bytes big-endian)
	buf.WriteString("JAVA PROFILE 1.0.2\x00")
	binary.Write(&buf, binary.BigEndian, uint32(8)) // 8-byte IDs

	// Timestamp (8 bytes)
	buf.Write(make([]byte, 8))

	// String record: stringID=1, value="Ljava/lang/String;"
	strContent := []byte("Ljava/lang/String;")
	recLen := uint32(8 + len(strContent)) // id(8) + content
	var rec [9]byte
	rec[0] = 0x01 // tagString
	// time
	binary.BigEndian.PutUint32(rec[1:5], 0)
	binary.BigEndian.PutUint32(rec[5:9], recLen)
	buf.Write(rec[:])
	binary.Write(&buf, binary.BigEndian, uint64(1)) // string ID
	buf.Write(strContent)

	// String record: stringID=2, value="[Ljava/lang/Object;"
	strContent2 := []byte("[Ljava/lang/Object;")
	recLen2 := uint32(8 + len(strContent2))
	rec[0] = 0x01
	binary.BigEndian.PutUint32(rec[5:9], recLen2)
	buf.Write(rec[:])
	binary.Write(&buf, binary.BigEndian, uint64(2))
	buf.Write(strContent2)

	// LoadClass record: serial=1, classObjID=10, stackTrace=0, classNameID=1
	rec[0] = 0x02 // tagLoadClass
	binary.BigEndian.PutUint32(rec[5:9], 4+8+4+8) // length
	buf.Write(rec[:])
	binary.Write(&buf, binary.BigEndian, uint32(1)) // class serial
	binary.Write(&buf, binary.BigEndian, uint64(10)) // class obj ID
	binary.Write(&buf, binary.BigEndian, uint32(0))  // stack trace serial
	binary.Write(&buf, binary.BigEndian, uint64(1))  // class name string ID

	// LoadClass record: serial=2, classObjID=20, classNameID=2
	rec[0] = 0x02
	binary.BigEndian.PutUint32(rec[5:9], 4+8+4+8)
	buf.Write(rec[:])
	binary.Write(&buf, binary.BigEndian, uint32(2))
	binary.Write(&buf, binary.BigEndian, uint64(20))
	binary.Write(&buf, binary.BigEndian, uint32(0))
	binary.Write(&buf, binary.BigEndian, uint64(2))

	// Heap dump segment
	var heapBuf bytes.Buffer

	// Instance dump: objID=100, stackTrace=0, classObjID=10, numFields=8 (8 bytes data)
	heapBuf.WriteByte(0x21) // subInstanceDump
	binary.Write(&heapBuf, binary.BigEndian, uint64(100)) // obj ID
	binary.Write(&heapBuf, binary.BigEndian, uint32(0))   // stack trace
	binary.Write(&heapBuf, binary.BigEndian, uint64(10))  // class obj ID
	binary.Write(&heapBuf, binary.BigEndian, uint32(8))   // num bytes
	heapBuf.Write(make([]byte, 8))                        // instance data

	// Instance dump: objID=101, stackTrace=0, classObjID=10, numFields=16
	heapBuf.WriteByte(0x21)
	binary.Write(&heapBuf, binary.BigEndian, uint64(101))
	binary.Write(&heapBuf, binary.BigEndian, uint32(0))
	binary.Write(&heapBuf, binary.BigEndian, uint64(10))
	binary.Write(&heapBuf, binary.BigEndian, uint32(16))
	heapBuf.Write(make([]byte, 16))

	// Instance dump: objID=102, stackTrace=0, classObjID=20, numFields=4
	heapBuf.WriteByte(0x21)
	binary.Write(&heapBuf, binary.BigEndian, uint64(102))
	binary.Write(&heapBuf, binary.BigEndian, uint32(0))
	binary.Write(&heapBuf, binary.BigEndian, uint64(20))
	binary.Write(&heapBuf, binary.BigEndian, uint32(4))
	heapBuf.Write(make([]byte, 4))

	// Primitive array dump: objID=200, stackTrace=0, numElements=1024, elemType=8(byte)
	heapBuf.WriteByte(0x23) // subPrimArrayDump
	binary.Write(&heapBuf, binary.BigEndian, uint64(200))
	binary.Write(&heapBuf, binary.BigEndian, uint32(0))
	binary.Write(&heapBuf, binary.BigEndian, uint32(1024)) // 1024 bytes
	heapBuf.WriteByte(8)                                    // byte type
	heapBuf.Write(make([]byte, 1024))

	// Heap dump end is a separate record, not inside the segment
	// Remove the heapBuf.WriteByte(0x2C) and let the segment end naturally

	// Write heap dump segment record
	segLen := uint32(heapBuf.Len())
	rec[0] = 0x1C // tagHeapDumpSeg
	binary.BigEndian.PutUint32(rec[5:9], segLen)
	buf.Write(rec[:])
	buf.Write(heapBuf.Bytes())

	return buf.Bytes()
}

func TestParseMinimalHPROF(t *testing.T) {
	data := buildMinimalHPROF()
	reader := bytes.NewReader(data)

	parser := NewHPROFParser()
	if err := parser.Parse(reader); err != nil {
		t.Fatalf("Parse failed: %v", err)
	}

	if len(parser.classes) != 2 {
		t.Errorf("expected 2 classes, got %d", len(parser.classes))
	}
	if parser.totalObj != 4 {
		t.Errorf("expected 4 objects, got %d", parser.totalObj)
	}
	if parser.totalSize != 8+16+4+1024 {
		t.Errorf("expected total size %d, got %d", 8+16+4+1024, parser.totalSize)
	}
}

func TestAnalyzeTopObjects(t *testing.T) {
	data := buildMinimalHPROF()
	reader := bytes.NewReader(data)

	parser := NewHPROFParser()
	if err := parser.Parse(reader); err != nil {
		t.Fatalf("Parse failed: %v", err)
	}

	result := parser.Analyze(10)
	if result.TotalObjects != 4 {
		t.Errorf("expected 4 total objects, got %d", result.TotalObjects)
	}
	if len(result.TopObjects) == 0 {
		t.Error("expected non-empty top objects")
	}
	// First entry should be byte[] (primarray, 1024 bytes)
	if result.TopObjects[0].TotalSize < 1024 {
		t.Errorf("top object should be >= 1024, got %d", result.TopObjects[0].TotalSize)
	}
}

func TestAnalyzeJSON(t *testing.T) {
	data := buildMinimalHPROF()
	reader := bytes.NewReader(data)

	parser := NewHPROFParser()
	if err := parser.Parse(reader); err != nil {
		t.Fatalf("Parse failed: %v", err)
	}

	result := parser.Analyze(5)
	out, err := json.Marshal(result)
	if err != nil {
		t.Fatalf("JSON marshal failed: %v", err)
	}
	if len(out) == 0 {
		t.Error("expected non-empty JSON output")
	}
}

func TestInvalidMagic(t *testing.T) {
	data := []byte("NOTAHPROFILE\x00" + string([]byte{0, 2, 8}))
	reader := bytes.NewReader(data)

	parser := NewHPROFParser()
	err := parser.Parse(reader)
	if err == nil {
		t.Error("expected error for invalid magic")
	}
}

func TestEmptyFile(t *testing.T) {
	reader := bytes.NewReader(nil)
	parser := NewHPROFParser()
	err := parser.Parse(reader)
	if err == nil {
		t.Error("expected error for empty file")
	}
}

func TestClassNameResolution(t *testing.T) {
	data := buildMinimalHPROF()
	reader := bytes.NewReader(data)

	parser := NewHPROFParser()
	if err := parser.Parse(reader); err != nil {
		t.Fatalf("Parse failed: %v", err)
	}

	cls, ok := parser.classes[10]
	if !ok {
		t.Fatal("class 10 not found")
	}
	if cls.ClassName != "java.lang.String" {
		t.Errorf("expected java.lang.String, got %s", cls.ClassName)
	}
}

func TestFileAnalysis(t *testing.T) {
	// Write minimal hprof to temp file and test main-like flow
	data := buildMinimalHPROF()
	tmpfile, err := os.CreateTemp("", "test-*.hprof")
	if err != nil {
		t.Fatal(err)
	}
	defer os.Remove(tmpfile.Name())
	tmpfile.Write(data)
	tmpfile.Close()

	f, err := os.Open(tmpfile.Name())
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()

	parser := NewHPROFParser()
	if err := parser.Parse(f); err != nil {
		t.Fatalf("Parse failed: %v", err)
	}

	result := parser.Analyze(10)
	if result.ClassCount != 2 {
		t.Errorf("expected 2 classes, got %d", result.ClassCount)
	}
}
