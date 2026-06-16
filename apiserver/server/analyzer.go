package server

import (
	"fmt"
	"os"
	"os/exec"
	"time"

	"github.com/drop/apiserver/model"
	"go.uber.org/zap"
)

// StartAnalysisScheduler launches a background goroutine that scans for
// tasks with status=DONE(3) and analysis_status=PENDING(0) every 10 seconds,
// spawns the analysis script, and updates the analysis_status.
func (s *APIServer) StartAnalysisScheduler() {
	go func() {
		s.logger.Info("analysis scheduler started (interval=10s)")
		ticker := time.NewTicker(10 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			s.runAnalysisTick()
		}
	}()
}

func (s *APIServer) runAnalysisTick() {
	var tasks []model.HotmethodTask
	// Find tasks: status=DONE, analysis_status=PENDING
	if err := s.db.Where("status = ? AND analysis_status = ?",
		model.TaskStatusDone, model.AnalysisStatusPending).
		Find(&tasks).Error; err != nil {
		s.logger.Error("analysis scheduler query", zap.Error(err))
		return
	}
	if len(tasks) == 0 {
		return
	}
	for _, t := range tasks {
		if t.CosKey == "" {
			s.logger.Warn("skipping task with no cos_key", zap.String("tid", t.TID))
			continue
		}
		s.spawnAnalysis(t.TID, t.ProfilerType)
	}
}

func (s *APIServer) spawnAnalysis(tid string, profilerType int) {
	// Mark as analyzing
	if err := s.updateAnalysisStatus(tid, model.AnalysisStatusRunning, "分析开始"); err != nil {
		s.logger.Error("mark analysis running", zap.String("tid", tid), zap.Error(err))
		return
	}

	cmd := exec.Command(
		"python3",
		"drop_analyzer/hotmethod_analyzer.py",
		"--task-id", tid,
		"--task-type", fmt.Sprintf("%d", profilerType),
		"--config", "config.ini",
	)
	cmd.Dir = "../analysis" // relative to apiserver binary location

	// If the working dir doesn't exist, try absolute path
	if _, err := os.Stat(cmd.Dir); err != nil {
		cmd.Dir = "/root/Drop/analysis"
	}

	output, err := cmd.CombinedOutput()
	if err != nil {
		reason := fmt.Sprintf("analysis failed: %v\n%s", err, string(output))
		s.logger.Error("analysis failed", zap.String("tid", tid), zap.Error(err))
		if uerr := s.updateAnalysisStatus(tid, model.AnalysisStatusFailed, reason); uerr != nil {
			s.logger.Error("mark analysis failed", zap.String("tid", tid), zap.Error(uerr))
		}
		return
	}
	s.logger.Info("analysis completed", zap.String("tid", tid), zap.String("output", string(output)))
}

func (s *APIServer) updateAnalysisStatus(tid string, status int, reason string) error {
	return s.db.Model(&model.HotmethodTask{}).
		Where("tid = ?", tid).
		Updates(map[string]interface{}{
			"analysis_status": status,
			"status_info":     reason,
		}).Error
}
