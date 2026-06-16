package server

import (
	"encoding/json"
	"time"

	"github.com/drop/apiserver/model"
	"github.com/robfig/cron/v3"
	"go.uber.org/zap"
)

// CronScheduler manages periodic continuous-profiling dispatches.
type CronScheduler struct {
	server *APIServer
	cron   *cron.Cron
	logger *zap.Logger
	entry  map[string]cron.EntryID // mt_tid -> entryID
}

// NewCronScheduler creates a scheduler backed by the APIServer.
func NewCronScheduler(s *APIServer) *CronScheduler {
	return &CronScheduler{
		server: s,
		cron:   cron.New(cron.WithSeconds()),
		logger: s.logger.Named("cron-scheduler"),
		entry:  make(map[string]cron.EntryID),
	}
}

// Start loads existing cron schedules from PG and starts the cron engine.
func (cs *CronScheduler) Start() {
	cs.loadExistingSchedules()
	cs.cron.Start()
	cs.logger.Info("cron scheduler started")
}

// Stop gracefully stops the cron engine.
func (cs *CronScheduler) Stop() {
	ctx := cs.cron.Stop()
	<-ctx.Done()
	cs.logger.Info("cron scheduler stopped")
}

// AddSchedule registers a new cron schedule for continuous profiling.
func (cs *CronScheduler) AddSchedule(mtTid, cronExpr string) error {
	// Remove existing schedule for this mt_tid if any
	cs.RemoveSchedule(mtTid)

	entryID, err := cs.cron.AddFunc(cronExpr, func() {
		cs.dispatchContinuousTask(mtTid)
	})
	if err != nil {
		return err
	}

	cs.entry[mtTid] = entryID
	cs.logger.Info("cron schedule added",
		zap.String("mt_tid", mtTid),
		zap.String("cron_expr", cronExpr),
		zap.Int("entry_id", int(entryID)),
	)
	return nil
}

// RemoveSchedule removes a cron schedule by mt_tid.
func (cs *CronScheduler) RemoveSchedule(mtTid string) {
	if entryID, ok := cs.entry[mtTid]; ok {
		cs.cron.Remove(entryID)
		delete(cs.entry, mtTid)
		cs.logger.Info("cron schedule removed", zap.String("mt_tid", mtTid))
	}
}

// loadExistingSchedules loads enabled cron schedules from PG on startup.
func (cs *CronScheduler) loadExistingSchedules() {
	var mts []model.MultiTasks
	cs.server.db.Where("trigger_type = ? AND enabled = ?", model.TriggerCron, true).Find(&mts)

	for _, mt := range mts {
		if mt.CronExpr == "" {
			continue
		}
		if err := cs.AddSchedule(mt.TID, mt.CronExpr); err != nil {
			cs.logger.Warn("failed to load cron schedule",
				zap.String("mt_tid", mt.TID),
				zap.String("cron_expr", mt.CronExpr),
				zap.Error(err),
			)
		}
	}
	cs.logger.Info("loaded existing cron schedules", zap.Int("count", len(cs.entry)))
}

// dispatchContinuousTask creates a new continuous profiling task based on the schedule config.
func (cs *CronScheduler) dispatchContinuousTask(mtTid string) {
	var mt model.MultiTasks
	if err := cs.server.db.Where("tid = ?", mtTid).First(&mt).Error; err != nil {
		cs.logger.Error("schedule mt not found", zap.String("mt_tid", mtTid), zap.Error(err))
		return
	}

	if !mt.Enabled {
		cs.logger.Info("schedule disabled, skipping", zap.String("mt_tid", mtTid))
		return
	}

	// Parse schedule params
	var params CreateTaskReq
	if mt.ScheduleParams != "" {
		json.Unmarshal([]byte(mt.ScheduleParams), &params)
	}

	// Force type=1 (continuous) for scheduled continuous profiling
	params.Type = model.TaskTypeContinuous

	tid := genTID()
	paramsJSON, _ := json.Marshal(params)

	task := model.HotmethodTask{
		TID:            tid,
		Name:           params.Name,
		Type:           model.TaskTypeContinuous,
		ProfilerType:   params.ProfilerType,
		TargetIP:       params.TargetIP,
		RequestParams:  string(paramsJSON),
		Status:         model.TaskStatusPending,
		StatusInfo:      "由定时规则自动创建",
		UID:            "cron-system",
		UserName:       "cron-scheduler",
		CreateTime:     model.Now(),
		MasterTaskTid:  mtTid,
	}

	if err := cs.server.db.Create(&task).Error; err != nil {
		cs.logger.Error("cron create task", zap.String("tid", tid), zap.Error(err))
		return
	}

	// Insert status history
	history := model.TaskStatusHistory{
		TID:       tid,
		OldStatus: -1,
		NewStatus: model.TaskStatusPending,
		Reason:    "由定时规则自动创建",
		Timestamp: task.CreateTime,
	}
	cs.server.db.Create(&history)

	// Append sub task TID
	var subTids []string
	json.Unmarshal([]byte(mt.SubTIDs), &subTids)
	subTids = append(subTids, tid)
	subJSON, _ := json.Marshal(subTids)
	cs.server.db.Model(&mt).Update("sub_tids", string(subJSON))

	// Dispatch via gRPC
	if cs.server.grpcCC != nil {
		go cs.server.dispatchTask(tid, params)
	} else {
		reason := "gRPC connection unavailable, cannot dispatch cron task"
		cs.logger.Warn("cannot dispatch cron task", zap.String("tid", tid))
		cs.server.updateTaskStatus(cs.server.db.Statement.Context, tid, model.TaskStatusFailed, reason)
	}

	cs.logger.Info("cron dispatched continuous task",
		zap.String("mt_tid", mtTid),
		zap.String("tid", tid),
	)
}

// nextRun returns the next scheduled time for display purposes.
func (cs *CronScheduler) nextRun(mtTid string) time.Time {
	if entryID, ok := cs.entry[mtTid]; ok {
		return cs.cron.Entry(entryID).Next
	}
	return time.Time{}
}
