package config

import (
	"fmt"

	"github.com/spf13/viper"
)

type Config struct {
	Server ServerConfig `mapstructure:"server"`
	PG     PGConfig     `mapstructure:"pg"`
	S3     S3Config     `mapstructure:"s3"`
	GRPC   GRPCConfig   `mapstructure:"grpc"`
	LLM    LLMConfig    `mapstructure:"llm"`
}

type ServerConfig struct {
	Port    string `mapstructure:"port"`
	DevMode bool   `mapstructure:"dev_mode"`
}

type PGConfig struct {
	DSN             string `mapstructure:"dsn"`
	MaxOpenConns    int    `mapstructure:"max_open_conns"`
	MaxIdleConns    int    `mapstructure:"max_idle_conns"`
	ConnMaxLifetime int    `mapstructure:"conn_max_lifetime_sec"` // seconds
}

type S3Config struct {
	Endpoint   string `mapstructure:"endpoint"`
	AccessKey  string `mapstructure:"access_key"`
	SecretKey  string `mapstructure:"secret_key"`
	Bucket     string `mapstructure:"bucket"`
	UseSSL     bool   `mapstructure:"use_ssl"`
	PreSignExp int    `mapstructure:"presign_exp_sec"` // seconds, default 3600
}

type GRPCConfig struct {
	Addr string `mapstructure:"addr"`
}

type LLMConfig struct {
	APIKey     string  `mapstructure:"api_key"`
	BaseURL    string  `mapstructure:"base_url"`
	Model      string  `mapstructure:"model"`
	MaxTokens  int     `mapstructure:"max_tokens"`
	Temperature float64 `mapstructure:"temperature"`
}

func Load(path string) (*Config, error) {
	viper.SetConfigFile(path)
	viper.AutomaticEnv()
	if err := viper.ReadInConfig(); err != nil {
		return nil, fmt.Errorf("read config: %w", err)
	}
	var cfg Config
	if err := viper.Unmarshal(&cfg); err != nil {
		return nil, fmt.Errorf("unmarshal config: %w", err)
	}
	// Defaults
	if cfg.PG.MaxOpenConns == 0 {
		cfg.PG.MaxOpenConns = 100
	}
	if cfg.PG.MaxIdleConns == 0 {
		cfg.PG.MaxIdleConns = 10
	}
	if cfg.PG.ConnMaxLifetime == 0 {
		cfg.PG.ConnMaxLifetime = 3600
	}
	if cfg.S3.PreSignExp == 0 {
		cfg.S3.PreSignExp = 3600
	}
	if cfg.S3.Bucket == "" {
		cfg.S3.Bucket = "drop-data"
	}
	if cfg.Server.Port == "" {
		cfg.Server.Port = ":8191"
	}
	return &cfg, nil
}
