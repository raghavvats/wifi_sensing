# Mesh TDMA orchestrator

Pi-side master clock for the ESP mesh CSI schedule.

```bash
set -a && source configs/sensing.env && set +a
# Mesh TDMA orchestrator (pi/orchestrator/run_mesh.py)
# Defaults: probe 20 ms, report 40 ms (see configs/sensing.env)
python3 pi/orchestrator/run_mesh.py
```

After a collect run:

```bash
python3 pi/collector/link_health.py pi/runs/<timestamp>.ndjson
```

See root [README.md](../../README.md) and [protocol/sched.md](../../protocol/sched.md).
