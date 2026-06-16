import { BrowserRouter, Routes, Route, Navigate } from 'react-router-dom';
import HomePage from './pages/home';
import TaskListPage from './pages/taskList';
import TaskResultPage from './pages/taskResult';

export default function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/" element={<HomePage />} />
        <Route path="/tasks" element={<TaskListPage />} />
        <Route path="/task/result" element={<TaskResultPage />} />
        <Route path="*" element={<Navigate to="/" replace />} />
      </Routes>
    </BrowserRouter>
  );
}
