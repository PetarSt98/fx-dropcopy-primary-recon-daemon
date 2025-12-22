# Incident Specifications

Incident specifications define deterministic replay inputs and expected outputs to support audit regression. Each incident lives under `incidents/<ID>/` and contains:

- `spec.json`: Incident definition (wire inputs and replay parameters).
- `golden/`: Golden outputs produced once from a trusted baseline build.
- `whitelist.json` (optional): Known-acceptable differences during comparisons.
- `README.md` (optional): Notes specific to the incident.

## Format (spec.json)

```json
{
  "id": "INC-2025-0001",
  "description": "EBS sequence gap handling bug",
  "wire_inputs": [
    {
      "path": "wire/ebs_20250101_120000.bin",
      "from_ns": 1735732800000000000,
      "to_ns": 1735732805000000000
    }
  ],
  "replay": {
    "speed": "fast",
    "max_records": 0
  }
}
```

- `wire_inputs`: One or more segments of wire logs to replay.
- `replay.speed`: `"fast"` for unthrottled playback (default), `"realtime"` for paced playback.
- `replay.max_records`: 0 means no limit.

## Workflow

1. **Generate golden outputs (run once):**

   ```bash
   replay_main --incident incidents/<ID>/spec.json \
               --config configs/baseline.json \
               --output incidents/<ID>/golden
   ```

2. **Test a candidate build (repeatable):**

   ```bash
   replay_main --incident incidents/<ID>/spec.json \
               --config configs/candidate.json \
               --output /tmp/candidate

   audit_diff incidents/<ID>/golden /tmp/candidate \
              --whitelist incidents/<ID>/whitelist.json
   ```

## Whitelist (whitelist.json)

```json
{
  "version": 1,
  "rules": [
    {
      "type": "ignore_divergence_type",
      "divergence_type": "QuantityMismatch",
      "reason": "Known rounding"
    },
    {
      "type": "ignore_n_occurrences",
      "divergence_type": "StateMismatch",
      "max_occurrences": 5,
      "reason": "Acceptable during restart"
    },
    {
      "type": "ignore_by_order_key",
      "order_keys": ["hash:123456789", "clordid:ABC123"],
      "reason": "Test orders"
    },
    {
      "type": "allow_extra_files",
      "patterns": ["*.tmp", "debug_*"],
      "reason": "Debug outputs"
    }
  ]
}
```

- Rules are evaluated in order. The first matching rule determines whether a difference is whitelisted.
- `order_keys` supports:
  - `hash:<uint64>`: exact audit key.
  - `clordid:<literal>`: hashed with the same FNV-1a algorithm used in audit records.
  - `<legacy>` strings like `EBS:12345` are hashed **including** the entire literal for backward compatibility.
