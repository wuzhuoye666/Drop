import axios from 'axios';

const api = axios.create({
  baseURL: '/api/v1',
  withCredentials: true,
});

// Auth
export const checkAuth = () => api.get('/auth/check');

// Agents
export const listAgents = () => api.get('/agents');
export const statAgent = (targetIp?: string) =>
  api.get('/agent/stat', { params: { target_ip: targetIp } });

// Tasks
export const createTask = (data: {
  name: string;
  type: number;
  profiler_type: number;
  target_ip: string;
  pid: number;
  duration: number;
  hz: number;
  event?: string;
}) => api.post('/tasks', data);

export const listTasks = (page = 1, pageSize = 20) =>
  api.get('/tasks', { params: { page, page_size: pageSize } });

export const getTask = (tid: string) => api.get(`/tasks/${tid}`);
export const deleteTask = (tid: string) => api.delete(`/tasks/${tid}`);
export const retryTask = (tid: string) => api.post(`/tasks/${tid}/retry`);

// Continuous Profiling Segments
export const listSegments = (tid: string) => api.get(`/tasks/${tid}/segments`);
export const getProfileWindow = (tid: string, start: number, end: number) =>
  api.get(`/tasks/${tid}/profile-window`, {
    params: { start, end },
    responseType: 'text',
  });

// Schedules
export const listSchedules = () => api.get('/schedules');
export const createScheduleTask = (data: {
  name: string;
  cron_expr: string;
  type: number;
  profiler_type: number;
  target_ip: string;
  pid: number;
  hz: number;
}) => api.post('/schedule/task', data);
export const toggleSchedule = (tid: string, enabled: boolean) =>
  api.put(`/schedule/${tid}/toggle`, { enabled });
export const deleteSchedule = (tid: string) => api.delete(`/schedule/${tid}`);

// COS files
export const listCosFiles = (tid: string) =>
  api.get('/cosfiles', { params: { tid } });

export default api;
