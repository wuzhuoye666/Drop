package main

import (
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

// HPROF record tags
const (
	tagString        = 0x01
	tagLoadClass     = 0x02
	tagUnloadClass   = 0x03
	tagStackFrame    = 0x04
	tagStackTrace    = 0x05
	tagAllocSites    = 0x06
	tagHeapSummary   = 0x07
	tagStartThread   = 0x0A
	tagEndThread     = 0x0B
	tagHeapDump      = 0x0C
	tagHeapDumpSeg   = 0x1C
	tagHeapDumpEnd   = 0x2C
)

// Heap dump sub-records
const (
	subRootUnknown    = 0xFF
	subRootJniGlobal  = 0x01
	subRootJniLocal   = 0x02
	subRootJavaFrame  = 0x03
	subRootNativeStack = 0x04
	subRootStickyClass = 0x05
	subRootThreadBlock = 0x06
	subRootMonitorUsed = 0x07
	subRootThreadObj  = 0x08
	subClassDump      = 0x20
	subInstanceDump   = 0x21
	subObjArrayDump   = 0x22
	subPrimArrayDump  = 0x23
)

// ClassInfo holds metadata for a loaded class
type ClassInfo struct {
	ClassSerialNum uint32
	ClassObjID     uint64
	ClassName      string
}

// HeapObject represents an object in the heap
type HeapObject struct {
	ID      uint64
	ClassID uint64
	Size    uint32
	Type    string // "instance", "objarray", "primarray"
}

// LeakSuspect is a potential memory leak
type LeakSuspect struct {
	ClassName string  `json:"class_name"`
	Count     int     `json:"count"`
	TotalSize int64   `json:"total_size"`
	Percent   float64 `json:"percent"`
}

// AnalysisResult is the final output
type AnalysisResult struct {
	TotalSize     int64         `json:"total_size"`
	TotalObjects  int           `json:"total_objects"`
	ClassCount    int           `json:"class_count"`
	TopObjects    []LeakSuspect `json:"top_objects"`
	BigObjects    []HeapObject  `json:"big_objects"`
}

// HPROFParser parses Java HPROF heap dump files
type HPROFParser struct {
	classes   map[uint64]*ClassInfo // classObjID -> ClassInfo
	objects   []HeapObject
	sizeByCls map[uint64]*clsAccum // classObjID -> accumulator
	totalSize int64
	totalObj  int
	idSize    int // 4 or 8 bytes per ID
}

type clsAccum struct {
	name      string
	count     int
	totalSize int64
}

// NewHPROFParser creates a new parser
func NewHPROFParser() *HPROFParser {
	return &HPROFParser{
		classes:   make(map[uint64]*ClassInfo),
		sizeByCls: make(map[uint64]*clsAccum),
	}
}

// Parse reads and parses an HPROF file
func (p *HPROFParser) Parse(r io.ReadSeeker) error {
	// Read header string (null-terminated)
	var headerBuf []byte
	for {
		var b [1]byte
		if _, err := io.ReadFull(r, b[:]); err != nil {
			return fmt.Errorf("reading hprof header: %w", err)
		}
		headerBuf = append(headerBuf, b[0])
		if b[0] == 0 {
			break
		}
	}
	headerStr := string(headerBuf[:len(headerBuf)-1]) // strip null
	if !strings.HasPrefix(headerStr, "JAVA PROFILE") {
		return fmt.Errorf("invalid hprof magic: %s", headerStr)
	}

	// Read ID size (4 bytes)
	var idSizeBuf [4]byte
	if _, err := io.ReadFull(r, idSizeBuf[:]); err != nil {
		return fmt.Errorf("reading id size: %w", err)
	}
	p.idSize = int(binary.BigEndian.Uint32(idSizeBuf[:]))
	if p.idSize != 4 && p.idSize != 8 {
		return fmt.Errorf("unexpected id size: %d", p.idSize)
	}

	// Read timestamp (8 bytes)
	var ts [8]byte
	if _, err := io.ReadFull(r, ts[:]); err != nil {
		return fmt.Errorf("reading timestamp: %w", err)
	}

	// Parse records
	for {
		// Tag (1 byte)
		var tag [1]byte
		if _, err := io.ReadFull(r, tag[:]); err != nil {
			if err == io.EOF {
				break
			}
			return fmt.Errorf("reading tag: %w", err)
		}

		// Time (4 bytes)
		var time [4]byte
		if _, err := io.ReadFull(r, time[:]); err != nil {
			return fmt.Errorf("reading time: %w", err)
		}

		// Length (4 bytes)
		var length [4]byte
		if _, err := io.ReadFull(r, length[:]); err != nil {
			return fmt.Errorf("reading length: %w", err)
		}
		recLen := binary.BigEndian.Uint32(length[:])

		switch tag[0] {
		case tagString:
			if err := p.parseString(r, recLen); err != nil {
				return err
			}
		case tagLoadClass:
			if err := p.parseLoadClass(r, recLen); err != nil {
				return err
			}
		case tagHeapDump, tagHeapDumpSeg:
			if err := p.parseHeapDump(r, recLen); err != nil {
				return err
			}
		default:
			// Skip unknown record
			if _, err := io.CopyN(io.Discard, r, int64(recLen)); err != nil {
				return fmt.Errorf("skipping record tag %x: %w", tag[0], err)
			}
		}
	}

	return nil
}

var stringMap = make(map[uint64]string) // stringID -> string

func (p *HPROFParser) parseString(r io.Reader, length uint32) error {
	buf := make([]byte, length)
	if _, err := io.ReadFull(r, buf); err != nil {
		return fmt.Errorf("reading string record: %w", err)
	}
	id := p.readID(buf[:p.idSize])
	s := string(buf[p.idSize:])
	stringMap[id] = s
	return nil
}

func (p *HPROFParser) parseLoadClass(r io.Reader, length uint32) error {
	buf := make([]byte, length)
	if _, err := io.ReadFull(r, buf); err != nil {
		return fmt.Errorf("reading load class record: %w", err)
	}
	off := 0
	classSerial := binary.BigEndian.Uint32(buf[off : off+4])
	off += 4
	classObjID := p.readID(buf[off : off+p.idSize])
	off += p.idSize
	// stack trace serial (4 bytes)
	off += 4
	classNameID := p.readID(buf[off : off+p.idSize])
	className := stringMap[classNameID]
	if strings.HasPrefix(className, "L") && strings.HasSuffix(className, ";") {
		className = className[1 : len(className)-1]
	}
	className = strings.ReplaceAll(className, "/", ".")

	p.classes[classObjID] = &ClassInfo{
		ClassSerialNum: classSerial,
		ClassObjID:     classObjID,
		ClassName:      className,
	}
	return nil
}

func (p *HPROFParser) parseHeapDump(r io.ReadSeeker, length uint32) error {
	remaining := int64(length)
	for remaining > 0 {
		var subtag [1]byte
		if _, err := io.ReadFull(r, subtag[:]); err != nil {
			return fmt.Errorf("reading subtag: %w", err)
		}
		remaining--

		switch subtag[0] {
		case subRootUnknown, subRootJniGlobal, subRootJniLocal,
			subRootJavaFrame, subRootNativeStack, subRootStickyClass,
			subRootThreadBlock, subRootMonitorUsed, subRootThreadObj:
			size := p.rootSubRecordSize(subtag[0])
			buf := make([]byte, size)
			if _, err := io.ReadFull(r, buf); err != nil {
				return fmt.Errorf("reading root sub-record %x: %w", subtag[0], err)
			}
			remaining -= int64(size)

		case subClassDump:
			size := p.classDumpSize()
			buf := make([]byte, size)
			if _, err := io.ReadFull(r, buf); err != nil {
				return fmt.Errorf("reading class dump: %w", err)
			}
			remaining -= int64(size)

		case subInstanceDump:
			obj, consumed, err := p.parseInstanceDump(r)
			if err != nil {
				return err
			}
			p.addObject(obj)
			remaining -= int64(consumed)

		case subObjArrayDump:
			obj, consumed, err := p.parseObjArrayDump(r)
			if err != nil {
				return err
			}
			p.addObject(obj)
			remaining -= int64(consumed)

		case subPrimArrayDump:
			obj, consumed, err := p.parsePrimArrayDump(r)
			if err != nil {
				return err
			}
			p.addObject(obj)
			remaining -= int64(consumed)

		default:
			return fmt.Errorf("unknown heap dump sub-record: 0x%02x at remaining=%d", subtag[0], remaining)
		}
	}
	return nil
}

func (p *HPROFParser) rootSubRecordSize(tag byte) int {
	base := int(p.idSize) + 4 // id + stacktrace
	switch tag {
	case subRootJniGlobal:
		return base + int(p.idSize) // + jni global ref id
	case subRootJniLocal, subRootJavaFrame:
		return base + 4 + 4 // + thread serial + frame
	case subRootNativeStack, subRootThreadBlock:
		return base + 4 // + thread serial
	case subRootThreadObj:
		return base + 4 + 4 // + thread serial + stack trace
	default:
		return base
	}
}

func (p *HPROFParser) classDumpSize() int {
	// classObjID + stacktrace(4) + superclassID + classloaderID +
	// signersID + protDomainID + reserved1 + reserved2 +
	// instanceSize(4) + constantPool(count2+entries) + statics(count2+entries) +
	// fields(count2+entries)
	// Minimum: 7*idSize + 4 + 4 + 4 + 4 + 4 + 4 = 7*idSize + 24
	return 7*p.idSize + 4 + 4 + 4 + 4 + 4 + 4
}

func (p *HPROFParser) parseInstanceDump(r io.Reader) (HeapObject, int, error) {
	header := make([]byte, p.idSize+4+p.idSize+4)
	if _, err := io.ReadFull(r, header); err != nil {
		return HeapObject{}, 0, fmt.Errorf("reading instance dump header: %w", err)
	}
	off := 0
 objID := p.readID(header[off : off+p.idSize])
	off += p.idSize
	// stack trace serial
	off += 4
	classObjID := p.readID(header[off : off+p.idSize])
	off += p.idSize
	numFields := binary.BigEndian.Uint32(header[off : off+4])
	off += 4

	instanceSize := numFields
	obj := HeapObject{
		ID:      objID,
		ClassID: classObjID,
		Size:    instanceSize,
		Type:    "instance",
	}

	// Skip instance data
	data := make([]byte, numFields)
	if _, err := io.ReadFull(r, data); err != nil {
		return HeapObject{}, 0, fmt.Errorf("reading instance data: %w", err)
	}

	consumed := len(header) + int(numFields)
	return obj, consumed, nil
}

func (p *HPROFParser) parseObjArrayDump(r io.Reader) (HeapObject, int, error) {
	header := make([]byte, p.idSize+4+4+p.idSize)
	if _, err := io.ReadFull(r, header); err != nil {
		return HeapObject{}, 0, fmt.Errorf("reading obj array dump header: %w", err)
	}
	off := 0
	arrayID := p.readID(header[off : off+p.idSize])
	off += p.idSize
	// stack trace serial
	off += 4
	numElements := binary.BigEndian.Uint32(header[off : off+4])
	off += 4
	elemClassID := p.readID(header[off : off+p.idSize])
	off += p.idSize

	size := uint32(numElements) * uint32(p.idSize)
	obj := HeapObject{
		ID:      arrayID,
		ClassID: elemClassID,
		Size:    size,
		Type:    "objarray",
	}

	// Skip element IDs
	data := make([]byte, int(numElements)*p.idSize)
	if _, err := io.ReadFull(r, data); err != nil {
		return HeapObject{}, 0, fmt.Errorf("reading obj array elements: %w", err)
	}

	consumed := len(header) + int(numElements)*p.idSize
	return obj, consumed, nil
}

func (p *HPROFParser) parsePrimArrayDump(r io.Reader) (HeapObject, int, error) {
	header := make([]byte, p.idSize+4+4+1)
	if _, err := io.ReadFull(r, header); err != nil {
		return HeapObject{}, 0, fmt.Errorf("reading prim array dump header: %w", err)
	}
	off := 0
	arrayID := p.readID(header[off : off+p.idSize])
	off += p.idSize
	// stack trace serial
	off += 4
	numElements := binary.BigEndian.Uint32(header[off : off+4])
	off += 4
	elemType := header[off]
	off++

	elemSize := primTypeSize(elemType)
	size := uint32(numElements) * uint32(elemSize)
	obj := HeapObject{
		ID:      arrayID,
		ClassID: 0,
		Size:    size,
		Type:    "primarray",
	}

	// Skip element data
	data := make([]byte, int(numElements)*elemSize)
	if _, err := io.ReadFull(r, data); err != nil {
		return HeapObject{}, 0, fmt.Errorf("reading prim array elements: %w", err)
	}

	consumed := len(header) + int(numElements)*elemSize
	return obj, consumed, nil
}

func (p *HPROFParser) addObject(obj HeapObject) {
	p.objects = append(p.objects, obj)
	p.totalSize += int64(obj.Size)
	p.totalObj++

	clsName := "unknown"
	if cls, ok := p.classes[obj.ClassID]; ok {
		clsName = cls.ClassName
	}
	acc, ok := p.sizeByCls[obj.ClassID]
	if !ok {
		acc = &clsAccum{name: clsName}
		p.sizeByCls[obj.ClassID] = acc
	}
	acc.count++
	acc.totalSize += int64(obj.Size)
}

func (p *HPROFParser) readID(buf []byte) uint64 {
	if p.idSize == 8 {
		return binary.BigEndian.Uint64(buf)
	}
	return uint64(binary.BigEndian.Uint32(buf))
}

func primTypeSize(t byte) int {
	switch t {
	case 4: // boolean
		return 1
	case 5: // char
		return 2
	case 6: // float
		return 4
	case 7: // double
		return 8
	case 8: // byte
		return 1
	case 9: // short
		return 2
	case 10: // int
		return 4
	case 11: // long
		return 8
	default:
		return 1
	}
}

// Analyze produces the analysis result
func (p *HPROFParser) Analyze(topN int) *AnalysisResult {
	if topN <= 0 {
		topN = 20
	}

	// Sort by total size descending
	suspects := make([]LeakSuspect, 0, len(p.sizeByCls))
	for _, acc := range p.sizeByCls {
		pct := 0.0
		if p.totalSize > 0 {
			pct = float64(acc.totalSize) * 100.0 / float64(p.totalSize)
		}
		suspects = append(suspects, LeakSuspect{
			ClassName: acc.name,
			Count:     acc.count,
			TotalSize: acc.totalSize,
			Percent:   pct,
		})
	}
	sort.Slice(suspects, func(i, j int) bool {
		return suspects[i].TotalSize > suspects[j].TotalSize
	})
	if len(suspects) > topN {
		suspects = suspects[:topN]
	}

	// Find big objects (> 1MB threshold)
	var bigObjs []HeapObject
	sort.Slice(p.objects, func(i, j int) bool {
		return p.objects[i].Size > p.objects[j].Size
	})
	for _, obj := range p.objects {
		if obj.Size < 1024*1024 { // < 1MB
			break
		}
		clsName := "unknown"
		if cls, ok := p.classes[obj.ClassID]; ok {
			clsName = cls.ClassName
		}
		obj.ClassID = 0 // clear ID, use ClassName instead if needed
		_ = clsName
		bigObjs = append(bigObjs, obj)
		if len(bigObjs) >= topN {
			break
		}
	}

	return &AnalysisResult{
		TotalSize:    p.totalSize,
		TotalObjects: p.totalObj,
		ClassCount:   len(p.classes),
		TopObjects:   suspects,
		BigObjects:   bigObjs,
	}
}

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: java_heap_analyzer <hprof-file> [top-n]\n")
		os.Exit(1)
	}

	filename := os.Args[1]
	topN := 20
	if len(os.Args) > 2 {
		fmt.Sscanf(os.Args[2], "%d", &topN)
	}

	f, err := os.Open(filename)
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error opening file: %v\n", err)
		os.Exit(1)
	}
	defer f.Close()

	parser := NewHPROFParser()
	if err := parser.Parse(f); err != nil {
		fmt.Fprintf(os.Stderr, "Error parsing hprof: %v\n", err)
		os.Exit(1)
	}

	result := parser.Analyze(topN)
	out, _ := json.MarshalIndent(result, "", "  ")
	fmt.Println(string(out))
}
