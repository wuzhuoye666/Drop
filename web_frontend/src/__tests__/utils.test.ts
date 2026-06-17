import { describe, it, expect } from 'vitest';
import { taskStatusLabel, taskStatusColor, analysisStatusLabel, analysisStatusColor, profilerTypeName } from '../utils';

describe('taskStatusLabel', () => {
  it('returns labels for all known statuses', () => {
    expect(taskStatusLabel(0)).toBe('PENDING');
    expect(taskStatusLabel(1)).toBe('RUNNING');
    expect(taskStatusLabel(2)).toBe('UPLOADING');
    expect(taskStatusLabel(3)).toBe('DONE');
    expect(taskStatusLabel(4)).toBe('FAILED');
  });

  it('returns "UNKNOWN" for unknown status', () => {
    expect(taskStatusLabel(99)).toBe('UNKNOWN');
  });
});

describe('taskStatusColor', () => {
  it('returns correct colors', () => {
    expect(taskStatusColor(0)).toBe('#8c8c8c');
    expect(taskStatusColor(3)).toBe('#52c41a');
    expect(taskStatusColor(4)).toBe('#ff4d4f');
  });
});

describe('analysisStatusLabel', () => {
  it('returns labels for all analysis statuses', () => {
    expect(analysisStatusLabel(0)).toBe('待分析');
    expect(analysisStatusLabel(1)).toBe('分析中');
    expect(analysisStatusLabel(2)).toBe('分析完成');
    expect(analysisStatusLabel(3)).toBe('分析失败');
  });
});

describe('analysisStatusColor', () => {
  it('returns correct processing color', () => {
    expect(analysisStatusColor(1)).toBe('#1890ff');
    expect(analysisStatusColor(2)).toBe('#52c41a');
  });
});

describe('profilerTypeName', () => {
  it('returns names for all profiler types', () => {
    expect(profilerTypeName(0)).toBe('CPU-perf');
    expect(profilerTypeName(1)).toBe('Java-async-profiler');
    expect(profilerTypeName(2)).toBe('Go-pprof');
    expect(profilerTypeName(3)).toBe('eBPF-offCPU');
  });

  it('returns formatted type for unknown type', () => {
    expect(profilerTypeName(99)).toBe('type-99');
  });
});
