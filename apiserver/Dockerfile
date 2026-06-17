FROM golang:1.25-bookworm AS builder

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -o /apiserver .

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl && rm -rf /var/lib/apt/lists/*
COPY --from=builder /apiserver /usr/local/bin/apiserver
COPY apiserver.yaml /etc/drop/apiserver.yaml

EXPOSE 8191
ENTRYPOINT ["apiserver", "-c", "/etc/drop/apiserver.yaml"]
