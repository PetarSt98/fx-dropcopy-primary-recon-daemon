# INC-2025-EXAMPLE

This example incident demonstrates the deterministic replay + audit comparison workflow.

## Files
- `spec.json`: Example incident definition.
- `whitelist.json`: Example whitelist exercising all rule types.

## Usage
1. Generate golden outputs from a trusted baseline:
   ```bash
   replay_main --incident incidents/INC-2025-EXAMPLE/spec.json \
               --config configs/baseline.json \
               --output incidents/INC-2025-EXAMPLE/golden
   ```
2. Test a candidate build and compare against the golden:
   ```bash
   replay_main --incident incidents/INC-2025-EXAMPLE/spec.json \
               --config configs/candidate.json \
               --output /tmp/candidate

   audit_diff incidents/INC-2025-EXAMPLE/golden /tmp/candidate \
              --whitelist incidents/INC-2025-EXAMPLE/whitelist.json
   ```
