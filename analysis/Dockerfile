FROM python:3.12-slim-bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    linux-perf perl curl && rm -rf /var/lib/apt/lists/*

COPY requirements.txt /tmp/
RUN pip install --no-cache-dir -r /tmp/requirements.txt && rm /tmp/requirements.txt

COPY . /opt/drop/analysis
COPY third_party/FlameGraph /opt/FlameGraph

ENV FLAMEGRAPH_PL=/opt/FlameGraph/flamegraph.pl
ENV STACKCOLLAPSE_PL=/opt/FlameGraph/stackcollapse-perf.pl
WORKDIR /opt/drop/analysis

ENTRYPOINT ["python3", "drop_analyzer/hotmethod_analyzer.py"]
