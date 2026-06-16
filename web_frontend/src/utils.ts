const STATUS_MAP: Record<number, { label: string; color: string }> = {
  0: { label: 'PENDING', color: '#8c8c8c' },
  1: { label: 'RUNNING', color: '#1890ff' },
  2: { label: 'UPLOADING', color: '#722ed1' },
  3: { label: 'DONE', color: '#52c41a' },
  4: { label: 'FAILED', color: '#ff4d4f' },
};

const ANALYSIS_MAP: Record<number, { label: string; color: string }> = {
  0: { label: '待分析', color: '#8c8c8c' },
  1: { label: '分析中', color: '#1890ff' },
  2: { label: '分析完成', color: '#52c41a' },
  3: { label: '分析失败', color: '#ff4d4f' },
};

const PROFILER_MAP: Record<number, string> = {
  0: 'CPU-perf',
  1: 'Java-async-profiler',
  2: 'Go-pprof',
  3: 'eBPF-offCPU',
};

export function taskStatusLabel(status: number) {
  return STATUS_MAP[status]?.label ?? 'UNKNOWN';
}

export function taskStatusColor(status: number) {
  return STATUS_MAP[status]?.color ?? '#8c8c8c';
}

export function analysisStatusLabel(status: number) {
  return ANALYSIS_MAP[status]?.label ?? 'UNKNOWN';
}

export function analysisStatusColor(status: number) {
  return ANALYSIS_MAP[status]?.color ?? '#8c8c8c';
}

export function profilerTypeName(type: number) {
  return PROFILER_MAP[type] ?? `type-${type}`;
}
